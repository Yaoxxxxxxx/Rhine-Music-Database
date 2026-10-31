#include "MusicLibrary.h"
#include "MusicData.h"
#include <QCoreApplication>
#include <QDataStream>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QThread>
#include <QTemporaryDir>
#include <QUrl>
#include <QDebug>

/**
 * 音乐库核心功能测试（本地版）
 *
 * 本地版来源恒为 local：登录、发现、收藏、在线搜索与在线播放流程已随功能移除
 * （T2/T9），本套件只保留不依赖在线来源的核心状态行为；本地库与歌词链路由
 * qt_local_music_regression 覆盖。
 */

namespace {
bool waitFor(const std::function<bool()> &ready, int timeout = 3000) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (!ready() && elapsed.elapsed() < timeout) {
        QCoreApplication::processEvents();
        QThread::msleep(1);
    }
    return ready();
}

QJsonObject song(QString id, QString title = "测试歌曲") {
    auto value = MusicData::item("song", id, title, "测试歌手");
    value["albumID"] = "7";
    value["album"] = "测试专辑";
    value["duration"] = 180000;
    value["cover"] = "https://music.126.net/fixture.jpg";
    return value;
}

// 创建测试用的静音音频文件
QString createTestAudio(QTemporaryDir &dir) {
    QFile audio(dir.filePath("test-silent.wav"));
    if (!audio.open(QIODevice::WriteOnly)) {
        return QString();
    }
    QDataStream wav(&audio);
    wav.setByteOrder(QDataStream::LittleEndian);
    constexpr quint32 bytes = 8000 * 2 * 4;
    wav.writeRawData("RIFF", 4);
    wav << quint32(36 + bytes);
    wav.writeRawData("WAVEfmt ", 8);
    wav << quint32(16) << quint16(1) << quint16(1) << quint32(8000) << quint32(16000) << quint16(2)
        << quint16(16);
    wav.writeRawData("data", 4);
    wav << bytes;
    wav.writeRawData(QByteArray(bytes, '\0').constData(), bytes);
    audio.close();
    return QUrl::fromLocalFile(audio.fileName()).toString();
}
}

bool runNativeSessionChecks(const QString &root, const QString &audioURL);

bool runNativeMusicChecks(const QString &root) {
    QFile file(root + "/slots.json");
    if (!file.open(QIODevice::ReadOnly)) {
        return false;
    }

    Host host(root, true);
    MusicLibrary library(&host, QJsonDocument::fromJson(file.readAll()).object());
    QTemporaryDir audioFixture;
    const auto audioURL = createTestAudio(audioFixture);
    if (audioURL.isEmpty()) {
        return false;
    }
    host.mediaControl("volume", 0);

    bool ok = true;
    int testCount = 0;
    const auto check = [&](bool value, const char *name) {
        testCount++;
        if (!value) {
            qCritical() << "Native music test failed:" << name;
            ok = false;
        }
    };

    // === 测试 1: 基础选择 ===
    library.choose({});
    check(library.current.isEmpty() && !library.detailLoading, "empty selection does not load metadata");

    auto testSong = song("1");
    library.choose(testSong);
    check(library.current["id"] == "1", "song selection");

    // === 测试 2: 播放队列状态 ===
    QJsonArray queue;
    for (int i = 1; i <= 5; ++i) {
        queue.append(song(QString::number(i)));
    }
    library.player.queue = queue;
    check(library.player.queue.size() == 5, "queue initialization");
    library.player.queue = QJsonArray{};
    check(library.player.queue.isEmpty(), "queue clears");

    // === 测试 3: 平台切换（本地版仅 local，在线来源一律拒绝）===
    const QString initialProvider = library.provider;
    library.switchProvider("qq");
    check(library.provider == initialProvider, "online provider switch is rejected");
    library.switchProvider("kugou");
    check(library.provider == initialProvider, "online provider switch is rejected (kugou)");
    library.switchProvider("local");
    check(library.provider == "local", "provider stays local");

    // === 测试 4: 音乐数据规范化 ===
    auto normalizedSong = MusicData::item("song", "test-id", "测试标题", "测试歌手");
    check(normalizedSong.contains("id") && normalizedSong.contains("title"), "music data structure");

    library.player.stop();
    ok &= runNativeSessionChecks(root, audioURL);
    host.shutdown();
    qInfo() << "Native music core tests:" << testCount << (ok ? "passed" : "FAILED");
    return ok;
}
