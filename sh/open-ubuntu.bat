@echo off
chcp 65001 >nul
:: ============================================
:: WSL Ubuntu 快捷启动器
:: 双击此文件即可打开Ubuntu并进入指定目录
:: ============================================

:: 方法1：使用Linux路径（推荐，路径是WSL内部的）
:: 示例：/home/miao_yang/projects
wsl -d Ubuntu --cd /mnt/d/01_work_project

:: 方法2：使用Windows路径（自动转换）
:: 示例：D:\projects 会变成 /mnt/d/projects
:: wsl -d Ubuntu --cd /mnt/d/projects

:: ============================================
:: 参数说明：
:: -d Ubuntu   指定发行版（你的就是Ubuntu）
:: --cd        启动后自动进入的目录
:: 
:: 修改方法：
:: 把上面 /home/miao_yang 改成你的实际路径
:: Linux路径示例：/home/miao_yang/work
:: Windows路径示例：/mnt/d/work （对应D:\work）
:: ============================================

pause