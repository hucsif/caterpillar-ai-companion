#!/bin/bash
# 毛毛虫智感 · 周报自动化（被 crontab 调用）
set -e
cd "$(dirname "$0")"
mkdir -p state out

echo "[$(date)] Generating weekly report..."
python3 weekly_report.py --days 7
echo "[$(date)] Weekly report done → out/weekly_report_$(date +%Y-%m-%d).txt"
