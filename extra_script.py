import shutil
import os
from pathlib import Path

# Import env từ SCons
Import("env")

# Lấy version từ Version.hpp
def get_version():
    version_file = Path("src/Version.hpp")
    with open(version_file, 'r') as f:
        for line in f:
            if 'APP_VERSION' in line and '=' in line:
                # Extract version from: static constexpr const char* APP_VERSION = "1.0.5";
                version = line.split('"')[1]
                return version
    return "unknown"

def after_build(source, target, env):
    # Tạo thư mục build nếu chưa tồn tại
    build_dir = Path("build")
    build_dir.mkdir(exist_ok=True)
    
    # Lấy phiên bản
    version = get_version()
    
    # Đường dẫn source firmware
    firmware_src = Path(".pio/build/esp32dev/firmware.bin")
    
    # Đường dẫn destination
    firmware_dst = build_dir / f"{version}.bin"
    
    if firmware_src.exists():
        shutil.copy(str(firmware_src), str(firmware_dst))
        print(f"\n✓ Firmware copied to: {firmware_dst}\n")

# Đăng ký hook
env.AddPostAction("$BUILD_DIR/${PROGNAME}.bin", after_build)
