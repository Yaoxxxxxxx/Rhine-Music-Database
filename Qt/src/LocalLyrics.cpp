#include "LocalLyrics.h"
#include <QJsonArray>
#include <QtEndian>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringConverter>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <iconv.h>
#endif

namespace LocalLyrics {
namespace {

/** 严格解码：QStringDecoder 遇到非法序列会插入 U+FFFD，以它作为失败标记。 */
bool decodeUnicode(QStringConverter::Encoding encoding, const QByteArray &bytes, QString &out) {
    QStringDecoder decoder(encoding);
    QString text = decoder.decode(bytes);
    if (!text.contains(QChar(0xFFFD))) {
        out = text;
        return true;
    }
    return false;
}

QString decodeUtf32(const QByteArray &bytes, bool littleEndian, bool *ok) {
    *ok = false;
    if (bytes.size() % 4 != 0) {
        return {};
    }
    QString text;
    text.reserve(bytes.size() / 4);
    for (int offset = 0; offset + 4 <= bytes.size(); offset += 4) {
        const auto point = littleEndian ? qFromLittleEndian<quint32>(bytes.constData() + offset)
                                        : qFromBigEndian<quint32>(bytes.constData() + offset);
        if (point > 0x10FFFF || (point >= 0xD800 && point <= 0xDFFF)) {
            return {};
        }
        const char32_t codePoint = static_cast<char32_t>(point);
        text.append(QString::fromUcs4(&codePoint, 1));
    }
    if (text.startsWith(QChar(0xFEFF))) {
        text.remove(0, 1);
    }
    *ok = true;
    return text;
}

/** 受控猜测的自检：无替换符、无异常控制符，含非 ASCII 时必须出现 CJK/假名。 */
bool plausible(const QString &text) {
    if (text.contains(QChar(0xFFFD))) {
        return false;
    }
    int nonAscii = 0;
    int cjk = 0;
    for (const auto &ch : text) {
        const auto code = ch.unicode();
        if (code == 0xFFFD) {
            return false;
        }
        if (code < 0x80) {
            if (code < 0x20 && code != '\t' && code != '\n' && code != '\r') {
                return false;
            }
            continue;
        }
        ++nonAscii;
        if ((code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3040 && code <= 0x30FF) ||
            (code >= 0x3400 && code <= 0x4DBF) || (code >= 0xF900 && code <= 0xFAFF) ||
            (code >= 0x3000 && code <= 0x303F) || (code >= 0xFF00 && code <= 0xFFEF)) {
            ++cjk;
        }
    }
    return nonAscii == 0 || cjk > 0;
}

#ifdef Q_OS_WIN
bool decodeWindowsCodePage(UINT codePage, const QByteArray &bytes, QString &out) {
    const int wideLength = MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.constData(),
                                               int(bytes.size()), nullptr, 0);
    if (wideLength <= 0) {
        return false;
    }
    std::wstring wide(size_t(wideLength), L'\0');
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()),
                            wide.data(), wideLength) <= 0) {
        return false;
    }
    out = QString::fromWCharArray(wide.data(), wideLength);
    return true;
}
#else
bool decodeIconv(const char *name, const QByteArray &bytes, QString &out) {
    iconv_t converter = iconv_open("UTF-8", name);
    if (converter == (iconv_t)-1) {
        return false;
    }
    char *inPointer = const_cast<char *>(bytes.constData());
    size_t inLeft = size_t(bytes.size());
    QByteArray result;
    result.reserve(bytes.size() * 2);
    char buffer[4096];
    while (inLeft > 0) {
        char *outPointer = buffer;
        size_t outLeft = sizeof(buffer);
        if (iconv(converter, &inPointer, &inLeft, &outPointer, &outLeft) == size_t(-1)) {
            iconv_close(converter);
            return false;
        }
        result.append(buffer, sizeof(buffer) - outLeft);
    }
    iconv_close(converter);
    out = QString::fromUtf8(result);
    return true;
}
#endif

/** 受控猜测候选：平台原生解码器 + 严格校验 + 合理性自检。 */
bool decodeLegacy(const QString &encoding, const QByteArray &bytes, QString &out) {
    QString text;
#ifdef Q_OS_WIN
    const UINT codePage = encoding == "gbk" ? 936 : encoding == "big5" ? 950 : 932;
    if (!decodeWindowsCodePage(codePage, bytes, text)) {
        std::fprintf(stderr, "DBG legacy decode failed: cp=%u\n", codePage);
        return false;
    }
#else
    const char *iconvName = encoding == "gbk" ? "GBK" : encoding == "big5" ? "BIG5" : "SHIFT_JIS";
    if (!decodeIconv(iconvName, bytes, text)) {
        return false;
    }
#endif
    if (!plausible(text)) {
        std::fprintf(stderr, "DBG legacy implausible: len=%d\n", int(text.size()));
        return false;
    }
    out = text;
    return true;
}
} // namespace

DecodeResult decode(const QByteArray &bytes, const QString &hint) {
    DecodeResult result;
    if (bytes.size() > maxContentBytes) {
        return result;
    }
    if (!hint.isEmpty()) {
        const auto name = hint.toLower();
        QString text;
        bool done = false;
        if (name == "utf-8" || name == "utf8") {
            QByteArray stripped = bytes;
            if (stripped.startsWith("\xEF\xBB\xBF")) {
                stripped.remove(0, 3);
            }
            done = decodeUnicode(QStringConverter::Utf8, stripped, text) && plausible(text);
            result.encoding = QStringLiteral("utf-8");
        } else if (name == "gbk") {
            done = decodeLegacy(QStringLiteral("gbk"), bytes, text);
            result.encoding = QStringLiteral("gbk");
        } else if (name == "big5") {
            done = decodeLegacy(QStringLiteral("big5"), bytes, text);
            result.encoding = QStringLiteral("big5");
        } else if (name == "shift-jis") {
            done = decodeLegacy(QStringLiteral("shift-jis"), bytes, text);
            result.encoding = QStringLiteral("shift-jis");
        }
        if (done) {
            result.text = text;
            result.ok = true;
        }
        return result;
    }

    // 一、证据判定：BOM。
    const auto prefix = bytes.left(4);
    bool ok = false;
    QString text;
    if (prefix.startsWith(QByteArrayLiteral("\xFF\xFE\x00\x00"))) {
        text = decodeUtf32(bytes.mid(4), true, &ok);
        if (ok) {
            result.encoding = QStringLiteral("utf-32le");
        }
    } else if (prefix.startsWith(QByteArrayLiteral("\x00\x00\xFE\xFF"))) {
        text = decodeUtf32(bytes.mid(4), false, &ok);
        if (ok) {
            result.encoding = QStringLiteral("utf-32be");
        }
    } else if (prefix.startsWith(QByteArrayLiteral("\xEF\xBB\xBF"))) {
        ok = decodeUnicode(QStringConverter::Utf8, bytes.mid(3), text);
        if (ok) {
            result.encoding = QStringLiteral("utf-8");
        }
    } else if (prefix.startsWith(QByteArrayLiteral("\xFF\xFE"))) {
        ok = decodeUnicode(QStringConverter::Utf16LE, bytes.mid(2), text);
        if (ok) {
            result.encoding = QStringLiteral("utf-16le");
        }
    } else if (prefix.startsWith(QByteArrayLiteral("\xFE\xFF"))) {
        ok = decodeUnicode(QStringConverter::Utf16BE, bytes.mid(2), text);
        if (ok) {
            result.encoding = QStringLiteral("utf-16be");
        }
    }
    if (ok) {
        result.text = text;
        result.ok = true;
        return result;
    }

    // 无 BOM 的 UTF-16 时间标签有稳定的交错零字节，不能先作为合法 UTF-8 接受。
    int zeros[2] = {0, 0};
    for (int index = 0; index < std::min<qsizetype>(bytes.size(), 128); ++index) {
        if (bytes.at(index) == '\0') {
            ++zeros[index % 2];
        }
    }
    if (zeros[1] >= 4 && zeros[1] > zeros[0] * 4) {
        if (decodeUnicode(QStringConverter::Utf16LE, bytes, text)) {
            result.text = text;
            result.encoding = QStringLiteral("utf-16le");
            result.ok = true;
            return result;
        }
    }
    if (zeros[0] >= 4 && zeros[0] > zeros[1] * 4) {
        if (decodeUnicode(QStringConverter::Utf16BE, bytes, text)) {
            result.text = text;
            result.encoding = QStringLiteral("utf-16be");
            result.ok = true;
            return result;
        }
    }

    // 严格 UTF-8 校验失败后才进入受控猜测，避免破坏正常歌词。
    if (decodeUnicode(QStringConverter::Utf8, bytes, text)) {
        result.text = text;
        result.encoding = QStringLiteral("utf-8");
        result.ok = true;
        return result;
    }

    // 二、受控猜测：GBK → Big5 → Shift-JIS。
    for (const auto &candidate : {QStringLiteral("gbk"), QStringLiteral("big5"),
                                  QStringLiteral("shift-jis")}) {
        if (decodeLegacy(candidate, bytes, text)) {
            result.text = text;
            result.encoding = candidate;
            result.ok = true;
            return result;
        }
    }
    return result;
}
} // namespace LocalLyrics
