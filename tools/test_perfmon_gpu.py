"""Compile and run GPU telemetry selection checks against a temporary sysfs tree (no GPU required)."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parent.parent

with tempfile.TemporaryDirectory(prefix='sw-perfmon-') as tmp:
    root = Path(tmp)
    sysfs = root / 'sys'
    for card, total in [('class/drm/card0', 536870912), ('class/drm/card1', 17179869184),
                        ('single/class/drm/card12', 1)]:
        device = sysfs / card / 'device'
        device.mkdir(parents=True)
        (device / 'gpu_busy_percent').write_text('5\n')
        (device / 'mem_info_vram_total').write_text(f'{total}\n')
    nodes = sysfs / 'dev/char'
    nodes.mkdir(parents=True)
    for node, card in [('226:128', 'card1'), ('226:129', 'card0')]:
        (nodes / node).symlink_to(sysfs / 'class/drm' / card, target_is_directory=True)
    exe = root / 'test_perfmon_gpu'
    subprocess.run(['clang', '-O1', '-ffunction-sections', '-fdata-sections',
                    str(ROOT / 'tools/test_perfmon_gpu.c'), '-Wl,--gc-sections',
                    '-lpthread', '-ldl', '-lm', '-o', str(exe)], check=True)
    subprocess.run([str(exe), str(sysfs)], check=True)
