#include "LocalLyrics.h"
#include <QCoreApplication>
#include <cstdio>

namespace {
int failures = 0;

void check(bool value, const char *name) {
    if (!value) {
        std::fprintf(stderr, "Local lyrics test failed: %s\n", name);
        ++failures;
    }
}
} // namespace

int main(int argc, char *argv[]) {
    QCoreApplication app(argc, argv);

    // 样本：UTF-8、带 BOM、GBK、Big5、Shift-JIS、逐字时间、损坏文件，另含
    // UTF-16LE（带 BOM）与无 BOM UTF-16BE（交错零字节探测）与人工指定编码。
    static const QByteArray k_utf8 =
        QByteArray::fromHex("5b74693ae6a0b7e69cace6ad8ce69bb25d0a5b61723ae6b58be8af95e6ad8ce6898b5d0a5b30303a"
                            "30312e30305de7acace4b880e58fa5e6ad8ce8af8d0a5b30303a30332e35305de7acace4ba8ce58f"
                            "a5e6ad8ce8af8d0a5b30303a30362e30305d456e64696e670a");
    static const QByteArray k_utf8bom =
        QByteArray::fromHex("efbbbf5b74693ae6a0b7e69cace6ad8ce69bb25d0a5b61723ae6b58be8af95e6ad8ce6898b5d0a5b"
                            "30303a30312e30305de7acace4b880e58fa5e6ad8ce8af8d0a5b30303a30332e35305de7acace4ba"
                            "8ce58fa5e6ad8ce8af8d0a5b30303a30362e30305d456e64696e670a");
    static const QByteArray k_utf16le = QByteArray::fromHex(
        "fffe5b00740069003a0037682c674c6bf2665d000a005b00610072003a004b6dd58b4c6b4b625d000a005b00300030003a00"
        "300031002e00300030005d002c7b004ee5534c6bcd8b0a005b00300030003a00300033002e00350030005d002c7b8c4ee553"
        "4c6bcd8b0a005b00300030003a00300036002e00300030005d0045006e00640069006e0067000a00");
    static const QByteArray k_utf16be_nobom = QByteArray::fromHex(
        "005b00740069003a6837672c6b4c66f2005d000a005b00610072003a6d4b8bd56b4c624b005d000a005b00300030003a0030"
        "0031002e00300030005d7b2c4e0053e56b4c8bcd000a005b00300030003a00300033002e00350030005d7b2c4e8c53e56b4c"
        "8bcd000a005b00300030003a00300036002e00300030005d0045006e00640069006e0067000a");
    static const QByteArray k_gbk = QByteArray::fromHex(
        "5b74693ad1f9b1beb8e8c7fa5d0a5b61723ab2e2cad4b8e8cad65d0a5b30303a30312e30305db5dad2bbbee4b8e8b4ca0a5b"
        "30303a30332e35305db5dab6febee4b8e8b4ca0a5b30303a30362e30305d456e64696e670a");
    static const QByteArray k_big5 = QByteArray::fromHex(
        "5b74693abccba5bbba71a6b15d0a5b61723ab4fab8d5ba71a4e25d0a5b30303a30312e30305db2c4a440a579ba71b5fc0a5b"
        "30303a30332e35305db2c4a447a579ba71b5fc0a5b30303a30362e30305d456e64696e670a");
    static const QByteArray k_sjis = QByteArray::fromHex(
        "5b74693a835483938376838b5d0a5b61723a8365835883675d0a5b30303a30312e30305d88ea8d7396da82cc89cc8e8c0a5b"
        "30303a30332e35305d93f18d7396da82cc89cc8e8c0a5b30303a30362e30305d456e64696e670a");
    static const QByteArray k_wordtiming =
        QByteArray::fromHex("5b30303a30312e30305de7acac203c30303a30312e33303ee4b880203c30303a30312e36303ee58f"
                            "a50a5b30303a30332e30305de7acace4ba8ce58fa50a");
    static const QByteArray k_corrupt = QByteArray::fromHex("5b30303a30312e30305dc483fffefa818c409a88fc9b");

    // 一、证据判定：各样本按正确编码解码，且文本可读。
    {
        const auto result = LocalLyrics::decode(k_utf8);
        check(result.ok && result.encoding == "utf-8" && result.text.contains("第一句歌词"),
              "utf-8 sample decodes without BOM");
    }
    {
        const auto result = LocalLyrics::decode(k_utf8bom);
        check(result.ok && result.encoding == "utf-8" && result.text.contains("第一句歌词"),
              "utf-8 BOM sample decodes");
    }
    {
        const auto result = LocalLyrics::decode(k_utf16le);
        check(result.ok && result.encoding == "utf-16le" && result.text.contains("第一句歌词"),
              "utf-16le BOM sample decodes");
    }
    {
        const auto result = LocalLyrics::decode(k_utf16be_nobom);
        check(result.ok && result.encoding == "utf-16be" && result.text.contains("第一句歌词"),
              "utf-16be without BOM detected by interleaved zero bytes");
    }
    {
        const auto result = LocalLyrics::decode(k_gbk);
        check(result.ok && result.encoding == "gbk" && result.text.contains("第一句歌词"),
              "gbk sample decodes via legacy tier");
    }
    {
        // 决策四的受控猜测不区分 GBK/Big5（高度重叠），Big5 字节可能被 GBK 先行
        // 解码为合法汉字；此处只断言安全解码（无替换符），编码纠偏归第三层人工指定。
        const auto result = LocalLyrics::decode(k_big5);
        check(result.ok && !result.text.contains(QChar(0xFFFD)),
              "big5 sample decodes safely via legacy tier");
    }
    {
        const auto result = LocalLyrics::decode(k_sjis);
        check(result.ok && !result.text.contains(QChar(0xFFFD)),
              "shift-jis sample decodes safely via legacy tier");
    }
    {
        const auto result = LocalLyrics::decode(k_wordtiming);
        check(result.ok && result.encoding == "utf-8" && result.text.contains("第二句"),
              "word timing sample stays valid utf-8");
    }

    // 二、损坏文件：任何层级都不得交付替换字符。
    {
        const auto result = LocalLyrics::decode(k_corrupt);
        check(!result.ok || !result.text.contains(QChar(0xFFFD)),
              "corrupt sample never yields replacement characters");
    }

    // 三、尺寸闸门。
    {
        const auto result = LocalLyrics::decode(QByteArray(int(LocalLyrics::maxContentBytes) + 1, 'a'));
        check(!result.ok, "oversized lyrics are rejected");
    }

    // 四、人工指定编码（第三层兜底）。
    {
        const auto forced = LocalLyrics::decode(k_gbk, "gbk");
        check(forced.ok && forced.encoding == "gbk" && forced.text.contains("第一句歌词"),
              "manual gbk hint decodes");
        const auto mismatch = LocalLyrics::decode(k_gbk, "shift-jis");
        check(!mismatch.ok || !mismatch.text.contains(QChar(0xFFFD)),
              "wrong manual hint fails safely without replacement characters");
    }

    if (failures != 0) {
        std::fprintf(stderr, "Local lyrics encoding tests: %d failures\n", failures);
        return 1;
    }
    std::printf("Local lyrics encoding tests passed\n");
    return 0;
}
