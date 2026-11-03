const path = require('node:path');
const fs = require('node:fs');
const { spawnSync } = require('node:child_process');
const { app, resourceDirectory } = require('./app-layout.cjs');
process.chdir(path.resolve(__dirname, '..'));
const mac = process.platform === 'darwin';
const win = process.platform === 'win32';
// 统一使用 python3：多数发行版不再提供 python 命令。
const python = process.env.PYTHON || (win ? 'python' : 'python3');
function run(command, args, env = {}) {
    const result = spawnSync(command, args, {
        stdio: 'inherit',
        env: {
            ...process.env,
            ...env,
        },
    });
    if (result.error) {
        throw result.error;
    }
    if (result.status !== 0) {
        process.exit(result.status || 1);
    }
}
run(process.execPath, ['scripts/check-repository.cjs']);
run(python, ['scripts/check-qt-project.py']);
run(process.execPath, ['scripts/check-music-upstream.cjs']);

if (win) {
    // Windows 上 ctest 的回归程序是链接 Qt 的 GUI 子系统程序：缺 Qt PATH 时不是命令行
    // 报错，而是每个测试弹一个“找不到 Qt6Gui.dll”的系统对话框（T-019 体检🟡）。
    const hasQtRuntime = (process.env.PATH || '').split(path.delimiter).some((dir) => {
        try {
            return fs.existsSync(path.join(dir.trim(), 'Qt6Gui.dll'));
        } catch {
            return false;
        }
    });
    if (!hasQtRuntime) {
        console.error('Qt 运行库不在 PATH（找不到 Qt6Gui.dll），ctest 将全部失败并弹系统对话框。');
        console.error(
            '请改用 D:\\tools\\ctest-local.bat（自动加载 vcvars64 与 Qt bin），或先把 Qt 的 bin 目录加入 PATH 再跑 npm test。',
        );
        process.exit(1);
    }
}

run('ctest', ['--test-dir', 'output/qt-build', '--output-on-failure']);
// 验证最终应用包
const resources = path.join(app, resourceDirectory);
run(python, ['scripts/audit-qt.py', app]);
run(process.execPath, ['scripts/check-packaged-app.cjs', app]);
console.log('Cross-platform offline regressions and application package audit passed');
