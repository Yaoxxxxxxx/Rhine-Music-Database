#include "AppStorage.h"
#include "LocalMusicLibrary.h"
#include "MusicData.h"
#include "MusicLibrary.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cstdio>
#include <functional>

/**
 * T9 用例 B/C：播放状态机与播放历史重启回归（本地版，argc<2 返回 1，第二参数选套件）。
 *
 * B（qt_playback_state_regression）：循环（列表环绕/单曲重复/队列末停止）、随机（不命中原曲目）、
 *   队列增删（插入下一首/同曲去重/移除/清空待播），并断言切歌后的当前曲目、队列长度与重复
 *   模式经会话存档（sessionState -> 全新播放器 restoreSessionState）重启后不漂移。
 * C（qt_playback_history_regression）：播放历史经产品持久化入口写入 -> 新播放器读回（含非法行
 *   过滤与 200 条上限）-> 清空持久化；本地索引同目录重建后完整读回。
 *
 * 队列/索引/模式的变更都是 play() 前段的同步成员操作，不依赖音频输出设备；wav 样本保证
 * 本地直读路径走通（无设备时 mediaLoad 失败不影响本套断言目标）。
 */

namespace {
bool waitFor(const std::function<bool()> &ready, int timeout = 10000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}
bool check(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "Playback state regression: %s\n", message);
    }
    return value;
}
/** 无标签 PCM wav（8kHz/16bit/单声道 2 秒）：TagLib 可读时长与采样率，ffmpeg 可直读。 */
bool writeWav(const QString &path) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QDataStream out(&file);
    out.setByteOrder(QDataStream::LittleEndian);
    const quint32 sampleCount = 16000;
    out.writeRawData("RIFF", 4);
    out << quint32(36 + sampleCount * 2);
    out.writeRawData("WAVEfmt ", 8);
    out << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2)
        << quint16(16);
    out.writeRawData("data", 4);
    out << quint32(sampleCount * 2);
    for (quint32 i = 0; i < sampleCount; ++i) {
        out << qint16(0);
    }
    return true;
}
QJsonObject track(const QString &id, const QString &wavPath) {
    auto value = MusicData::item("song", id, "状态曲目 " + id, "本地歌手", "local");
    value["format"] = "wav";
    value["url"] = QUrl::fromLocalFile(wavPath).toString();
    return value;
}
/** 用例 B：循环、随机、队列增删与重启不漂移。 */
bool runStateChecks(const QString &root) {
    QTemporaryDir storage;
    bool ok = check(AppStorage::initializePortable(storage.path()).isEmpty(), "isolated storage");
    Host host(root, true);
    host.offline = false;
    const auto content = QJsonDocument::fromJson(host.read("slots.json").toUtf8()).object();
    QTemporaryDir audioDir;
    const QStringList names{"a", "b", "c", "d"};
    for (const auto &name : names) {
        ok &= check(writeWav(audioDir.filePath(name + ".wav")), "create state fixture");
    }
    MusicLibrary library(&host, content);
    auto &player = library.player;
    player.volume(0);
    QJsonArray source;
    for (const auto &name : names) {
        source.append(track(name, audioDir.filePath(name + ".wav")));
    }
    player.play(track("b", audioDir.filePath("b.wav")), source);
    ok &= check(player.index == 1 && player.queue.size() == 4 && player.mode == "all" &&
                    player.current["id"].toString() == "b",
                "play builds queue, selection and default repeat mode");

    // 队列增删：插入下一首；同曲再次入队去重并挪到队尾；移除当前无效；移除前方曲目收缩队列。
    const auto inserted = track("e", audioDir.filePath("a.wav"));
    player.enqueue(inserted, true);
    ok &= check(player.queue.size() == 5 &&
                    player.queue.at(player.index + 1).toObject()["id"].toString() == "e",
                "enqueue next inserts right after the current track");
    player.enqueue(inserted, false);
    ok &= check(player.queue.size() == 5 &&
                    player.queue.last().toObject()["id"].toString() == "e" &&
                    player.queue.at(player.index + 1).toObject()["id"].toString() == "c",
                "re-enqueue deduplicates and moves the track to the end");
    const auto sizeBeforeRemoval = player.queue.size();
    player.remove(player.index);
    ok &= check(player.queue.size() == sizeBeforeRemoval && player.current["id"].toString() == "b",
                "removing the playing track is a no-op");
    player.remove(0);
    ok &= check(player.queue.size() == sizeBeforeRemoval - 1 && player.index == 0 &&
                    player.current["id"].toString() == "b",
                "removing an earlier track shrinks the queue and keeps the current one");

    // 列表循环：末曲向前环绕；单曲循环：ended 原地重播；mode off：队列末明确停止。
    player.setMode("all");
    player.play(track("e", audioDir.filePath("e.wav")), std::nullopt, 3);
    player.next(1);
    ok &= check(player.index == 0 && player.current == player.queue.at(0).toObject(),
                "list loop wraps past the last track");
    player.setMode("one");
    player.next(1, true);
    ok &= check(player.index == 0 && player.current == player.queue.at(0).toObject(),
                "single-track repeat replays in place on end");
    player.setMode("off");
    player.play(track("e", audioDir.filePath("e.wav")), std::nullopt, 3);
    player.next(1);
    ok &= check(player.index == 3 && player.status.contains("队列已播放完"),
                "queue end stops and reports in mode off");

    // 随机：不命中原曲目，当前曲目与索引一致。
    player.setMode("shuffle");
    player.next(1);
    ok &= check(player.index != 3 && player.index >= 0 && player.index < player.queue.size() &&
                    player.current == player.queue.at(player.index).toObject(),
                "shuffle picks a different queue entry consistently");

    // 重启不漂移：会话存档交给全新播放器恢复（restoreSessionState 无返回值，以恢复结果断言），
    // 当前曲目、队列、重复模式逐一比对；恢复失败会直接表现为这里不等。
    const auto saved = player.sessionState();
    MusicLibrary restarted(&host, content);
    restarted.player.restoreSessionState(saved);
    ok &= check(restarted.player.mode == player.mode && restarted.player.index == player.index &&
                    restarted.player.queue == player.queue && restarted.player.current == player.current,
                "current track, queue length and repeat mode survive restart");
    restarted.player.stop();
    player.stop();
    host.shutdown();
    std::fprintf(stderr, "Playback state regression: %s\n", ok ? "passed" : "FAILED");
    return ok;
}
/** 用例 C：播放历史持久化读回（过滤/上限/清空）＋本地索引重启读回。 */
bool runHistoryChecks(const QString &root) {
    QTemporaryDir storage;
    bool ok = check(AppStorage::initializePortable(storage.path()).isEmpty(), "isolated storage");
    Host host(root, true);
    host.offline = false;
    const auto content = QJsonDocument::fromJson(host.read("slots.json").toUtf8()).object();

    // 经产品持久化入口写入历史（与播放入库同键），混入 kind 非 song 的非法行验证读回过滤。
    QJsonArray seeded;
    for (int i = 0; i < 5; ++i) {
        seeded.append(MusicData::item("song", QStringLiteral("history-%1").arg(i), "历史曲目",
                                      "本地歌手", "local"));
    }
    QJsonObject invalid{{"kind", "album"}, {"id", "not-a-song"}, {"provider", "local"}};
    seeded.append(invalid);
    host.setPreference("play-history",
                       QString::fromUtf8(QJsonDocument(seeded).toJson(QJsonDocument::Compact)));
    {
        MusicLibrary library(&host, content);
        ok &= check(library.player.history.size() == 5 &&
                        library.player.history.first().toObject()["id"].toString() == "history-0" &&
                        library.player.history.last().toObject()["id"].toString() == "history-4",
                    "play history is read back in order with invalid rows filtered");
    }
    // 200 条上限：写入 205 条，读回恰好 200 条且保留最近（数组前段）。
    QJsonArray overflow;
    for (int i = 0; i < 205; ++i) {
        overflow.append(MusicData::item("song", QString::number(i), "历史曲目", "本地歌手", "local"));
    }
    host.setPreference("play-history",
                       QString::fromUtf8(QJsonDocument(overflow).toJson(QJsonDocument::Compact)));
    {
        MusicLibrary library(&host, content);
        ok &= check(library.player.history.size() == 200 &&
                        library.player.history.first().toObject()["id"].toString() == "0",
                    "history caps at 200 keeping the most recent entries");
    }
    // 清空走产品入口并落盘。
    {
        MusicLibrary library(&host, content);
        library.player.clearHistory();
        ok &= check(library.player.history.isEmpty(), "clear history empties the in-memory list");
    }
    ok &= check(host.preference("play-history") == "[]", "clear history persists the empty list");

    // 本地索引重启读回：同一索引目录重建库，曲目集合不变。
    QTemporaryDir audioDir;
    QTemporaryDir indexDir;
    ok &= check(writeWav(audioDir.filePath("one.wav")), "create history-suite fixture one");
    ok &= check(writeWav(audioDir.filePath("two.wav")), "create history-suite fixture two");
    {
        LocalMusicLibrary local(indexDir.path());
        local.importPaths({audioDir.path()});
        ok &= check(waitFor([&] { return !local.busy; }), "history-suite fixtures indexed");
        ok &= check(local.songs().size() == 2, "local index built for restart check");
    }
    {
        LocalMusicLibrary restored(indexDir.path());
        ok &= check(restored.songs().size() == 2, "local index survives restart");
    }
    host.shutdown();
    std::fprintf(stderr, "Playback history regression: %s\n", ok ? "passed" : "FAILED");
    return ok;
}
}  // namespace

int main(int argc, char **argv) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QCoreApplication app(argc, argv);
    if (argc < 2) {
        return 1;
    }
    const auto root = QString::fromLocal8Bit(argv[1]);
    const auto mode = argc >= 3 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("state");
    const bool ok = mode == "history" ? runHistoryChecks(root) : runStateChecks(root);
    return ok ? 0 : 1;
}
