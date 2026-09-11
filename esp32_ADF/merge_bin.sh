#!/bin/bash

# 首先检查 IDF_PATH 环境变量
if [ -z "$IDF_PATH" ]; then
    echo "Error: IDF_PATH is not set. Please source export.sh first."
    exit 1
fi

# 激活 ESP-IDF 虚拟环境
. $IDF_PATH/export.sh

# 检查必要的文件
if [ ! -f ./build/flash_args ]; then
    echo "Error: flash_args file not found."
    exit 1
fi

# 切换到构建目录并执行合并
cd build/

# 读取 flash_args
flash_args=$(cat flash_args)

# 获取版本号(可选)
version=$(cat ../version.txt)
# 设置输出文件名,包含项目名和版本号
date=$(date +%Y%m%d)
output_filename="vib_player_v${version}_${date}_full_firmware.bin"

# 使用完整路径执行 esptool.py
python $IDF_PATH/components/esptool_py/esptool/esptool.py --chip esp32 merge_bin -o "${output_filename}" ${flash_args}

# 显示 MD5 校验和
printf "\nmerge bin completed, md5sum:\n"
md5sum "${output_filename}"

# 复制到 firmware 目录
mkdir -p ../firmware/
cp "${output_filename}" ../firmware/

# 返回原目录
cd ..