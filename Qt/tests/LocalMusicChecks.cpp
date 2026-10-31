#include "LocalMusicLibrary.h"
#include "MusicLibrary.h"
#include "LyricsController.h"
#include "AppStorage.h"
#include "NativeHud.h"
#include <QAudioBufferOutput>
#include <QAudioBuffer>
#include <QCoreApplication>
#include <QDataStream>
#include <QDir>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonDocument>
#include <QMediaDevices>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <cmath>

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
bool write(const QString &path, const QByteArray &content) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(content) == content.size();
}
QByteArray chunk(const char *name, QByteArray payload) {
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData(name, 4);
    stream << quint32(payload.size());
    stream.writeRawData(payload.constData(), payload.size());
    if (payload.size() % 2) {
        stream << quint8(0);
    }
    return bytes;
}
/** 生成真实 PCM 与 RIFF INFO 标签，直接验证解析器和解码器，不依赖在线音乐或用户文件。 */
QByteArray wave(QString title) {
    QByteArray format;
    QDataStream stream(&format, QIODevice::WriteOnly);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2) << quint16(16);
    QByteArray samples;
    QDataStream pcm(&samples, QIODevice::WriteOnly);
    pcm.setByteOrder(QDataStream::LittleEndian);
    for (int i = 0; i < 32000; ++i) {
        pcm << qint16(800 * std::sin(i * 440.0 * 2 * 3.141592653589793 / 8000));
    }
    const auto info = QByteArray("INFO") + chunk("INAM", title.toUtf8() + '\0') +
                      chunk("IART", QByteArray("Fixture Artist\0", 15)) +
                      chunk("IPRD", QByteArray("Fixture Album\0", 14));
    const auto body =
        QByteArray("WAVE") + chunk("fmt ", format) + chunk("LIST", info) + chunk("data", samples);
    return chunk("RIFF", body);
}
QJsonObject document(QString source = "netease") {
    return {{"version", 1},
            {"source", source},
            {"timing", "line"},
            {"offsetMs", 0},
            {"plainText", ""},
            {"lines", QJsonArray{QJsonObject{{"startMs", 0}, {"text", "one"}},
                                 QJsonObject{{"startMs", 1000}, {"text", "two"}}}}};
}
}

class AudioPlayerRegression {
  public:
    static bool decode(const QString &path) {
        AudioPlayer audio;
        audio.output.setVolume(0);
        audio.enableDiagnostics(true);
        const auto loaded = audio.load(QUrl::fromLocalFile(path).toString(), false, false, "local");
        // 创建独立媒体会话
        auto &capture = *audio.decodedOutput;
        int frames = 0;
        qint64 bufferPosition = -1;
        bool nonzero = false;
        QObject::connect(&capture, &QAudioBufferOutput::audioBufferReceived, &capture,
                         [&](const QAudioBuffer &buffer) {
                             frames += buffer.frameCount();
                             bufferPosition = buffer.startTime() / 1000;
                             const auto bytes = QByteArrayView(buffer.constData<char>(), buffer.byteCount());
                             for (const auto byte : bytes) {
                                 nonzero |= byte != 0;
                             }
                         });
        const auto decoded =
            loaded && waitFor([&] { return frames > 4000 && nonzero && audio.player->position() >= 200; });
        const auto position = audio.player->position();
        audio.control("pause");
        const auto paused = audio.player->playbackState() == QMediaPlayer::PausedState;
        const auto framesBeforeSeek = frames;
        bufferPosition = -1;
        audio.control("seek", 1800);
        audio.control("resume");
        const auto seeked = waitFor([&] {
            return frames > framesBeforeSeek && bufferPosition >= 1700 && audio.player->position() >= 1900;
        });
        // 在清理音源前验证实际编解码器
        const auto diagnostic = audio.diagnosticState();
        audio.control("stop");
        qInfo() << "Local decoder: frames" << frames << "nonzero" << nonzero << "position" << position
                << "seek buffer position" << bufferPosition;
        const bool diagnosticDecoded =
            diagnostic["decodedFrames"].toDouble() > 4000 && diagnostic["sampleRateHz"].toInt() > 0 &&
            diagnostic["codec"].toInt(-1) >= 0 && diagnostic["decodedBytes"].toDouble() > 0;
        return decoded && paused && seeked && diagnosticDecoded;
    }
};

int main(int argc, char **argv) {
    qputenv("QT_MEDIA_BACKEND", "ffmpeg");
    QGuiApplication app(argc, argv);
    if (argc < 2) {
        return 1;
    }
    bool ok = true;
    const auto check = [&](bool result, const char *name) {
        if (!result) {
            qCritical() << "Local music regression:" << name;
            ok = false;
        }
    };
    // T9 用例 A（qt_local_music_real_regression）：--real-dir 标记走真实目录回归，目录取环境
    // 变量 RHINE_REAL_MUSIC_DIR；未设置时退出 77，ctest 以 SKIP_RETURN_CODE 记 skipped，不误报失败。
    // 显式第二参数仍按原样视为目录。可选真实目录验收始终使用临时索引，不写回或移动用户音乐。
    QString realDir;
    if (argc >= 3 && QByteArray(argv[2]) == "--real-dir") {
        realDir = qEnvironmentVariable("RHINE_REAL_MUSIC_DIR");
        if (realDir.isEmpty()) {
            qInfo() << "RHINE_REAL_MUSIC_DIR not set; real directory regression skipped";
            return 77;
        }
    } else if (argc >= 3) {
        realDir = QString::fromLocal8Bit(argv[2]);
    }
    if (!realDir.isEmpty()) {
        QTemporaryDir realStorage;
        LocalMusicLibrary real(realStorage.path());
        real.importPaths({realDir});
        check(waitFor([&] { return !real.busy; }, 120000), "real directory scan completes");
        const auto tracks = real.songs();
        check(!tracks.isEmpty(), "real directory contains valid tracks");
        int covers = 0;
        int lyrics = 0;
        int decoded = 0;
        for (const auto &value : tracks) {
            const auto track = value.toObject();
            covers += !track["cover"].toString().isEmpty();
            const auto path = track["path"].toString();
            lyrics += QFileInfo::exists(QFileInfo(path).absolutePath() + '/' +
                                        QFileInfo(path).completeBaseName() + ".lrc");
            check(track["duration"].toInt() > 0 && track["sampleRate"].toInt() > 0,
                  "real track has valid duration and sample rate");
            check(!real.search(track["title"].toString(), "song").isEmpty(), "real local title search");
            if (!QMediaDevices::defaultAudioOutput().isNull()) {
                const auto result = AudioPlayerRegression::decode(path);
                check(result, qPrintable("decode " + QFileInfo(path).fileName()));
                decoded += result;
            }
        }
        real.rescan();
        check(waitFor([&] { return !real.busy; }, 120000) && real.songs() == tracks,
              "real incremental scan preserves index");
        qInfo() << "REAL LIBRARY tracks" << tracks.size() << "covers" << covers << "sidecars" << lyrics
                << "decoded" << decoded << real.status;
        return ok ? 0 : 1;
    }
    QTemporaryDir temporary;
    const auto untagged = temporary.filePath("Singer,Guest - Untagged.wav");
    check(write(untagged, wave("")), "create filename fallback fixture");
    LocalMusicLibrary filenameLibrary({}, true);
    filenameLibrary.importPaths({untagged});
    check(waitFor([&] { return !filenameLibrary.busy; }), "filename fixture indexed");
    const auto inferred = filenameLibrary.songs().first().toObject();
    check(inferred["title"] == "Untagged", "missing title inferred from filename");
    const auto root = temporary.filePath("music");
    const auto storage = temporary.filePath("index");
    const auto first = root + "/album/one.wav";
    check(write(first, wave("First Song")), "create tagged fixture");
    check(write(root + "/album/two.wav", wave("Second Song")), "create nested fixture");
    check(write(root + "/album/skipped.wav", wave("Skip")), "create ignored fixture");
    check(write(root + "/album/bad.mp3", "not audio"), "create invalid fixture");
    check(write(root + "/.foliaignore", "**/skipped.wav\n*.wav\n!**/one.wav\n!**/two.wav\n"),
          "create ordered ignore rules");
    QImage cover(64, 64, QImage::Format_RGB32);
    cover.fill(Qt::darkGreen);
    check(cover.save(root + "/album/cover.png"), "create folder cover");
    LocalMusicLibrary local(storage);
    local.importPaths({root, root + "/album"});
    check(waitFor([&] { return !local.busy; }), "background import completes");
    check(local.songs().size() == 2 && local.roots().size() == 2, "recursive import and overlapping dedupe");
    const auto song = local.search("First", "song").first().toObject();
    check(song["title"] == "First Song" && song["artist"] == "Fixture Artist" &&
              song["album"] == "Fixture Album" && song["duration"].toInt() == 4000,
          "real TagLib metadata");
    // T7：本地库不再提取封面，曲目与合集均无 cover 字段。
    check(song["cover"].toString().isEmpty() && local.collections("album").size() == 1 &&
              local.collections("folder").size() == 1 && local.collections("artist").size() == 1,
          "distinct collection types without cover extraction");
    qInfo() << "Indexed tracks:" << local.songs().size() << local.status;
    const auto originalId = song["id"];
    local.importPaths({root});
    check(waitFor([&] { return !local.busy; }) && local.roots().size() == 2 &&
              local.status.contains("复用 2"),
          "repeat import reuses unchanged metadata");
    check(write(first, wave("Changed Song")), "modify audio tags");
    check(QFile::remove(root + "/album/two.wav"), "remove fixture file");
    local.rescan();
    check(waitFor([&] { return !local.busy; }) && local.songs().size() == 1 &&
              local.songs().first().toObject()["title"] == "Changed Song" &&
              local.songs().first().toObject()["id"] == originalId,
          "incremental update and deleted file removal preserve path identity");
    LocalMusicLibrary restored(storage);
    check(restored.songs() == local.songs(), "index survives restart");
    check(QDir().rename(root, root + "-away"), "simulate unavailable root");
    local.rescan();
    check(waitFor([&] { return !local.busy; }) && local.songs().size() == 1 &&
              local.status.contains("不可访问"),
          "unavailable root retains old index");
    check(QDir().rename(root + "-away", root), "restore root");
    local.rescan();
    local.cancel();
    check(!local.busy && local.songs().size() == 1, "cancel preserves committed index");
    local.removeRoot(0);
    check(local.songs().size() == 1 && QFileInfo::exists(first), "overlapping root retains referenced song");
    local.removeRoot(0);
    check(local.songs().isEmpty() && QFileInfo::exists(first), "remove only index, retain original audio");

    Host host(app.arguments().at(1), true);
    QFile sceneContent(host.root + "/slots.json");
    check(sceneContent.open(QIODevice::ReadOnly), "load independent scene content");
    MusicLibrary music(&host, QJsonDocument::fromJson(sceneContent.readAll()).object());
    QStringList requests;
    bool missingFirstCover = false;
    host.fixture = [&](QString route, QJsonObject) {
        requests.append(route);
        // T5b 起本地歌词不再经桥（/local/lyrics 路由已废弃）；若将来有代码重新发出，
        // 下文 "no bridge round-trip" 断言会拦截，这里不再提供桥的假响应。
        const auto operation = route.section('/', 3, 3);
        QJsonValue response;
        if (operation == "search") {
            auto candidate = song;
            candidate["provider"] = "netease";
            candidate["id"] = "7";
            candidate["cover"] = missingFirstCover && route.contains("netease")
                                     ? QString()
                                     : QStringLiteral("https://music.126.net/local-fixture.png");
            response = QJsonObject{{"items", QJsonArray{candidate}}};
        } else if (operation == "lyrics") {
            response = document();
        }
        return QJsonObject{{"body", QJsonObject{{"code", 200}, {"value", response}}}};
    };
    music.switchProvider("local");
    host.fixtureImage("https://music.126.net/local-fixture.png?param=1024y1024", cover);
    music.importLocal({root});
    check(waitFor([&] { return !music.local->busy; }), "library imports local fixture");
    music.search("Changed");
    check(music.lists[0].size() == 1 && requests.isEmpty(),
          "local search and selection issue no online request");
    music.startLogin();
    music.restoreAccount();
    music.restoreIdentityName();
    music.playOfficial();
    check(!music.editable() && !music.canMutate("/like") && music.provider == "local",
          "local mode disables account writes and official delivery");
    requests.clear();
    const auto track = music.lists[0].first().toObject();
    check(write(root + "/album/one.lrc", "[00:00.00]one\n[00:01.00]two\n"), "create sidecar lyrics");
    music.player.play(track);
    check(waitFor([&] { return music.player.lyrics.status == "ready"; }) && requests.isEmpty(),
          "local cover and sidecar lyrics parse locally without any request");
    music.player.stop();
    requests.clear();
    check(QFile::remove(root + "/album/one.lrc"), "remove sidecar lyrics");
    missingFirstCover = true;
    auto missing = song;
    missing["cover"] = "";
    missing["localLyrics"] = "";
    music.player.lyrics.load(missing);
    // T7：本地曲目缺歌词直接 empty，封面匹配已整体移除（无任何在线封面动作）。
    check(waitFor([&] { return music.player.lyrics.status == "empty"; }) &&
              !requests.contains("/local/lyrics"),
          "missing local metadata yields empty state, no bridge round-trip, no cover matching");
    music.switchProvider("qq");
    check(music.provider == "local" && music.local->songs().size() == 1,
          "online provider switch is rejected and local index retained");
    // 缺失本地歌词：状态为 empty 而非 error，且不发起任何请求（T5b）。
    // 幽灵路径：该文件从不存在，断言不受导入映射状态影响。
    auto noLyrics = song;
    noLyrics["provider"] = "local";
    noLyrics["path"] = root + "/album/ghost.wav";
    noLyrics["cover"] = "";
    noLyrics["localLyrics"] = "";
    requests.clear();
    music.player.lyrics.load(noLyrics);
    check(waitFor([&] { return music.player.lyrics.status == "empty"; }) && requests.isEmpty(),
          "local track without lyrics yields empty state without requests");
    // === T6 歌词手动导入：导入 → 逐句 ready；映射持久化 → 重启生效；源删除 → 回退 empty。
    const QString importedLrc = root + "/album/imported.lrc";
    check(write(importedLrc, "[00:01.00]导入的第一句\n[00:03.00]导入的第二句\n"),
          "create imported lyrics file");
    auto importTarget = song;
    importTarget["provider"] = "local";
    importTarget["path"] = first;
    importTarget["cover"] = "";
    importTarget["localLyrics"] = "";
    requests.clear();
    music.player.lyrics.importLyricsFor(importTarget, importedLrc);
    check(waitFor([&] { return music.player.lyrics.status == "ready"; }) && requests.isEmpty() &&
              music.player.lyrics.document()["lines"].toArray().size() == 2,
          "imported lyrics parse locally and become ready");
    {
        // 重启生效：新控制器从映射 JSON 读回导入关系（T6 验收）。
        LyricsController fresh(&host);
        fresh.load(importTarget);
        check(waitFor([&] { return fresh.status == "ready"; }) &&
                  fresh.document()["lines"].toArray().size() == 2,
              "imported lyrics survive restart via mapping file");
    }
    check(QFile::remove(importedLrc), "remove imported lyrics source");
    music.player.lyrics.load(importTarget);
    check(waitFor([&] { return music.player.lyrics.status == "empty"; }),
          "missing imported source falls back to empty");
    // 清理映射文件，保证后续运行与其它检查不被导入状态污染。
    QFile::remove(AppStorage::dataDirectory() + QStringLiteral("/local-lyric-imports.json"));
    music.player.lyrics.load(noLyrics);
    if (QMediaDevices::defaultAudioOutput().isNull()) {
        qWarning() << "Local audio output unavailable; decoder/output check not verified";
    } else {
        check(AudioPlayerRegression::decode(first),
              "real FFmpeg samples, pause and seek through local AudioPlayer");
    }
    host.shutdown();
    qInfo() << "Local library, provider isolation, metadata priority and playback:" << ok;
    return ok ? 0 : 1;
}
