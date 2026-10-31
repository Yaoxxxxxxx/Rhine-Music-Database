// 按显式白名单组装干净的应用，禁止整目录复制暂存区。
// T8：桥与 Node 运行时不再打包，白名单仅保留本地版所需资源。
const fs = require('node:fs');
const path = require('node:path');
const root = path.resolve('output/resources');
const out = path.resolve(process.argv[2]);
const files = [
    'archive-baked-surface.bin',
    'archive-baked-light.bin',
    'archive-baked-normal.bin',
    'archive-cassette.bin',
    'archive-cassette-indices.bin',
    'archive-cassette.json',
    'archive-assembly.bin',
    'archive-assembly-indices.bin',
    'archive-assembly.json',
    'NotoSansSymbols2-Regular.ttf',
    'slots.json',
    'scene-fixture.json',
    'boot-lettering-art.json',
    ...[
        'netease-sq',
        'netease-hires',
        'netease-jymaster',
        'netease-jyeffect',
        'netease-sky',
        'qq-hq',
        'qq-sq',
        'kugou-hq',
        'kugou-sq',
        'kugou-hires',
    ].map((name) => `quality/${name}.png`),
    'quality/sources.json',
    'misans-glyphs.json',
    ...['light', 'regular', 'demibold', 'bold'].map((w) => `RhineMiSans-${w}.otf`),
];
fs.mkdirSync(out, {
    recursive: true,
});
for (const name of files) {
    fs.mkdirSync(path.dirname(path.join(out, name)), { recursive: true });
    fs.copyFileSync(path.join(root, name), path.join(out, name));
}
fs.copyFileSync('NOTICE', path.join(out, 'NOTICE'));
// 许可证原样复制，不转换字体或替换资源。
fs.copyFileSync('licenses/NotoSansSymbols2-OFL.txt', path.join(out, 'NotoSansSymbols2-OFL.txt'));
fs.copyFileSync('licenses/MiSans-license.pdf', path.join(out, 'MiSans-license.pdf'));
console.log(`Clean resources: ${files.length} files`);
