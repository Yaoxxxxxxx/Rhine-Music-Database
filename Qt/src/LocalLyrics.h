#pragma once
#include <QByteArray>
#include <QJsonObject>
#include <QString>

namespace LocalLyrics {
inline constexpr int maxContentBytes = 1000000;

struct DecodeResult {
    QString text;
    QString encoding;
    bool ok = false;
};

/**
 * 本地歌词三层编码兜底：
 * 一、证据判定：BOM（UTF-32/UTF-8/UTF-16）、无 BOM 的 UTF-16 交错零字节、严格 UTF-8；
 * 二、受控猜测：GBK（Windows 代码页 936）→ Big5（950）→ Shift-JIS（932），解码后自检
 *     （不得含 U+FFFD 或异常控制符，含非 ASCII 时必须出现 CJK/假名）；
 * 三、人工指定：hint 非空时只按指定编码（utf-8/gbk/big5/shift-jis）重解。
 * 对应上游 MusicBridge/lyrics-text.cjs 的证据判定层；chardet 全量探测不复刻（决策四）。
 */
DecodeResult decode(const QByteArray &bytes, const QString &hint = {});

/**
 * LRC 解析与归一化（T5b）：移植 MusicBridge/lyrics-format.cjs 的 local 分支，
 * 产出与在线歌词同构的 document（version/source/timing/offsetMs/lines/plainText，
 * 另含 encoding）。支持多时间标签、【翻译】后缀、角色署名过滤、增强 LRC 词时间
 * （行内 <mm:ss.xx>，timing=word）。解析失败返回空对象（不含 version）。
 */
QJsonObject normalize(const QString &text, const QString &encoding = {});
QJsonObject documentFromBytes(const QByteArray &bytes, const QString &hint = {});
}
