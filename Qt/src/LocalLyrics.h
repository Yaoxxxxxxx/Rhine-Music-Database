#pragma once
#include <QByteArray>
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
}
