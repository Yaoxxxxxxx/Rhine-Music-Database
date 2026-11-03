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
    const int wideLength =
        MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()), nullptr, 0);
    if (wideLength <= 0) {
        return false;
    }
    std::wstring wide(size_t(wideLength), L'\0');
    if (MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, bytes.constData(), int(bytes.size()), wide.data(),
                            wideLength) <= 0) {
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
        return false;
    }
#else
    const char *iconvName = encoding == "gbk" ? "GBK" : encoding == "big5" ? "BIG5" : "SHIFT_JIS";
    if (!decodeIconv(iconvName, bytes, text)) {
        return false;
    }
#endif
    if (!plausible(text)) {
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
    for (const auto &candidate :
         {QStringLiteral("gbk"), QStringLiteral("big5"), QStringLiteral("shift-jis")}) {
        if (decodeLegacy(candidate, bytes, text)) {
            result.text = text;
            result.encoding = candidate;
            result.ok = true;
            return result;
        }
    }
    return result;
}
namespace {
QString cleanText(const QString &value) {
    auto text = value;
    text.replace(QStringLiteral("&lt;"), QLatin1String("<"));
    text.replace(QStringLiteral("&gt;"), QLatin1String(">"));
    text.replace(QStringLiteral("&quot;"), QLatin1String("\""));
    text.replace(QStringLiteral("&apos;"), QLatin1String("'"));
    static const QRegularExpression numericEntity(QStringLiteral("&#(x[0-9a-fA-F]+|\\d+);"));
    qsizetype at = 0;
    while (true) {
        const auto match = numericEntity.match(text, at);
        if (!match.hasMatch()) {
            break;
        }
        const auto code = match.captured(1);
        const bool hex = code.at(0).toLower() == u'x';
        bool valid = false;
        const auto point = hex ? code.mid(1).toULongLong(&valid, 16) : code.toULongLong(&valid, 10);
        QString replacement = match.captured(0);
        if (valid && point <= 0x10FFFF) {
            char32_t codePoint = static_cast<char32_t>(point);
            replacement = QString::fromUcs4(&codePoint, 1);
        }
        text.replace(match.capturedStart(0), match.capturedLength(0), replacement);
        at = match.capturedStart(0) + replacement.size();
    }
    text.replace(QStringLiteral("&amp;"), QLatin1String("&"));
    text.replace(QRegularExpression(QStringLiteral("\r\n?")), QLatin1String("\n"));
    return text.left(maxContentBytes);
}

QJsonObject parseTags(const QString &content) {
    QJsonObject tags;
    static const QRegularExpression tagLine(QStringLiteral("^\\s*\\[([a-z]+):([^]\\n]*)\\]\\s*$"),
                                            QRegularExpression::CaseInsensitiveOption |
                                                QRegularExpression::MultilineOption);
    auto iterator = tagLine.globalMatch(content);
    while (iterator.hasNext()) {
        const auto match = iterator.next();
        const auto key = match.captured(1).toLower();
        if (!tags.contains(key)) {
            tags.insert(key, match.captured(2).trimmed());
        }
    }
    return tags;
}

struct LrcLine {
    double startMs = 0;
    QString text;
    QString translation;
    QJsonArray words;
};

QJsonArray lrcLinesToJson(const std::vector<LrcLine> &lines) {
    QJsonArray array;
    for (const auto &line : lines) {
        QJsonObject value{{"startMs", line.startMs},
                          {"endMs", QJsonValue()},
                          {"text", line.text},
                          {"translation", line.translation},
                          {"words", line.words}};
        if (!line.words.isEmpty()) {
            value["endMs"] = line.words.last().toObject()["endMs"].toDouble();
        }
        array.append(value);
    }
    return array;
}
} // namespace

QJsonObject normalize(const QString &rawText, const QString &encoding) {
    const auto content = cleanText(rawText);
    static const QRegularExpression lineStamps(
        QStringLiteral("^\\s*((?:\\[-?\\d+:\\d+(?:[.:]\\d+)?\\])+)(.*)$"));
    static const QRegularExpression stampToken(QStringLiteral("\\[([-+]?\\d+):(\\d+)(?:[.:](\\d+))?\\]"));
    static const QRegularExpression translationSuffix(QStringLiteral("【(.*)】$"));
    static const QRegularExpression wordTag(QStringLiteral("<(\\d+):(\\d+)(?:[.:](\\d+))?>"));

    std::vector<LrcLine> lines;
    const auto rows = content.split(QLatin1Char('\n'));
    for (const auto &row : rows) {
        const auto stamped = lineStamps.match(row);
        if (!stamped.hasMatch()) {
            continue;
        }
        auto body = stamped.captured(2);
        QString translation;
        const auto translated = translationSuffix.match(body);
        if (translated.hasMatch()) {
            translation = translated.captured(1);
            body = body.left(translated.capturedStart(1)).trimmed();
        } else {
            body = body.trimmed();
        }
        std::vector<LrcLine> expanded;
        auto stamps = stampToken.globalMatch(stamped.captured(1));
        while (stamps.hasNext()) {
            const auto stamp = stamps.next();
            const double minutes = stamp.captured(1).toDouble();
            const double seconds = stamp.captured(2).toDouble();
            const double fraction =
                stamp.captured(3).isEmpty() ? 0.0 : (QStringLiteral("0.") + stamp.captured(3)).toDouble();
            const auto startMs = qRound((minutes * 60 + seconds) * 1000 + fraction * 1000);
            LrcLine line;
            line.startMs = startMs;
            line.translation = translation;
            auto words = wordTag.globalMatch(body);
            if (words.hasNext()) {
                // 增强 LRC 词时间：首个词段归行起点，后续词段归各自标签时间。
                QString remainder = body;
                double wordStart = startMs;
                QJsonArray wordArray;
                QString wordText;
                while (words.hasNext()) {
                    const auto word = words.next();
                    const auto spoken = remainder.left(word.capturedStart(0));
                    wordText += spoken;
                    if (!wordArray.isEmpty()) {
                        wordArray.append(QJsonObject{{"startMs", wordStart},
                                                     {"endMs", word.captured(1).toDouble() * 60000 +
                                                                   word.captured(2).toDouble() * 1000},
                                                     {"text", spoken.trimmed()}});
                    }
                    wordStart = word.captured(1).toDouble() * 60000 + word.captured(2).toDouble() * 1000;
                    remainder = remainder.mid(word.capturedEnd(0));
                }
                wordText = wordText.trimmed();
                if (!wordArray.isEmpty()) {
                    line.words = wordArray;
                    line.text = wordText;
                    expanded.push_back(line);
                    continue;
                }
                line.text = wordText;
                expanded.push_back(line);
                continue;
            }
            line.text = body;
            expanded.push_back(line);
        }
        for (auto &line : expanded) {
            lines.push_back(line);
        }
    }
    std::stable_sort(lines.begin(), lines.end(),
                     [](const LrcLine &left, const LrcLine &right) { return left.startMs < right.startMs; });

    const auto tags = parseTags(content);
    // 只移除具有明确角色或文件头身份的署名行（LyricsCore 语义）。
    static const QRegularExpression creditRole(
        QStringLiteral("^(作词|作曲|编曲|制谱|演唱|制作人|录音棚|录音师|混音师|母带制作|民谣吉他|"
                       "电吉他|手风琴|第一小提琴|第二小提琴|中提琴|大提琴|词|曲|lyrics by|"
                       "composed by|written by|produced by|arranged by)(?:\\s+[^:：]+)?\\s*[:：]"),
        QRegularExpression::CaseInsensitiveOption);
    const auto title = tags.value("ti").toString().trimmed();
    const auto artist = tags.value("ar").toString().trimmed();
    lines.erase(std::remove_if(lines.begin(), lines.end(),
                               [&](const LrcLine &line) {
                                   const auto text = line.text.trimmed();
                                   if (creditRole.match(text).hasMatch()) {
                                       return true;
                                   }
                                   return !title.isEmpty() && !artist.isEmpty() &&
                                          text == title + QLatin1String(" - ") + artist;
                               }),
                lines.end());

    bool offsetOk = false;
    const auto offsetValue = tags.value("offset").toString().toDouble(&offsetOk);
    const auto offsetMs = offsetOk && std::abs(offsetValue) <= 10000 ? offsetValue : 0.0;

    bool hasContent = false;
    QSet<double> uniqueStarts;
    bool wordTimed = false;
    for (const auto &line : lines) {
        uniqueStarts.insert(line.startMs);
        if (!line.words.isEmpty()) {
            wordTimed = true;
        }
        const auto trimmed = line.text.trimmed();
        if (trimmed.isEmpty() || creditRole.match(trimmed).hasMatch() ||
            trimmed.startsWith(QStringLiteral("纯音乐")) ||
            trimmed.startsWith(QStringLiteral("此歌曲为没有填词的纯音乐"))) {
            continue;
        }
        hasContent = true;
    }
    const bool timed = hasContent && uniqueStarts.size() > 1;
    QString plainText;
    if (!timed) {
        static const QRegularExpression tagLineOnly(QStringLiteral("^\\[[a-z]+:.*\\]\\s*$"),
                                                    QRegularExpression::CaseInsensitiveOption |
                                                        QRegularExpression::MultilineOption);
        static const QRegularExpression anyBracket(QStringLiteral("\\[[^]\\n]+\\]"));
        plainText = content;
        plainText.remove(tagLineOnly);
        plainText.remove(anyBracket);
        plainText = plainText.trimmed();
    }

    QJsonObject document{{"version", 1},
                         {"source", QStringLiteral("local")},
                         {"timing", timed ? (wordTimed ? QStringLiteral("word") : QStringLiteral("line"))
                                          : QStringLiteral("none")},
                         {"offsetMs", offsetMs},
                         {"lines", timed ? lrcLinesToJson(lines) : QJsonArray()},
                         {"plainText", plainText}};
    if (!encoding.isEmpty()) {
        document["encoding"] = encoding;
    }
    return document;
}

QJsonObject documentFromBytes(const QByteArray &bytes, const QString &hint) {
    const auto decoded = decode(bytes, hint);
    if (!decoded.ok || decoded.text.trimmed().isEmpty()) {
        return {};
    }
    return normalize(decoded.text, decoded.encoding);
}

} // namespace LocalLyrics
