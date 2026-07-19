# xiaozhi-esp32 股票桌面摆件 — 变更记录

> 每次修改代码都追加更新此文档。最后更新：2025-07-12

---

## 一、项目概况

- **硬件**：Waveshare ESP32-S3-RLCD-4.2（400x300 单色反射屏）
- **框架**：ESP-IDF v5.5.4 + LVGL v9.4
- **音频**：BoxAudioCodec（喇叭）
- **MCU**：ESP32-S3（8MB PSRAM）

---

## 二、已修改的文件

### 1. `sdkconfig`
- 启用 `CONFIG_LV_USE_CHART=y`（备用，当前未使用）

### 2. `stock_ui.cc`（新建）
- **功能**：股票页 UI 布局（纯文字版）
- **布局**：
  - 顶部指数栏（上证 + 深证）
  - 左侧自选股列表（5只，每只显示名称+价格）
  - 右侧选中股票详情（价格、涨跌、今开昨收、最高最低、成交额、涨跌额）
  - 底部创业板指
- **状态栏**：由系统自动显示（时钟、WiFi、电池），股票页不重复

### 3. `custom_lcd_display.h`（修改）
- 新增 `MODE_STOCK = 3` 枚举
- 新增股票页成员变量：
  - `stock_page_` — 页面容器
  - `stock_list_items_[10]` — 列表项 label
  - `stock_list_[10]` — 股票数据（code + name）
  - `stock_list_count_` / `stock_current_index_` — 计数和选中索引
  - `stock_index_top_label_` — 顶部指数栏
  - `stock_name_label_` / `stock_price_label_` — 名称和价格
  - `stock_info_label_` / `stock_high_low_label_` — 今开昨收、最高最低
  - `stock_amount_label_` / `stock_change_label_` — 成交额、涨跌额
  - `stock_index_bottom_label_` — 底部创业板指
- 新增方法声明：
  - `SetupStockUI()` — 创建 UI
  - `SwitchToStockPage()` — 切换到股票页
  - `SwitchToNextStock()` — 切换下一只股票
  - `AddStock(code, name)` — 添加股票
  - `UpdateStockLabels(...)` — 更新股票标签
  - `UpdateStockIndexLabels(...)` — 更新指数标签

### 4. `custom_lcd_display.cc`（修改）
- 构造函数：添加 `SetupStockUI()` 调用
- `ApplyDisplayMode`：添加 `MODE_STOCK` case
- `CycleDisplayMode`：四页循环（天气→音乐→番茄钟→股票→天气）
- 新增 `SwitchToStockPage()` / `SwitchToNextStock()` / `AddStock()`
- 新增 `UpdateStockLabels()` — 更新股票详情
- 新增 `UpdateStockIndexLabels()` — 更新指数

### 5. `data_update_task.cc`（修改）
- 添加腾讯财经 API 数据获取：
  - URL：`http://qt.gtimg.cn/q=sh000001,sz399001,sz399006,sh600519,sz000001,sh601318,sz300750,sz002594`
  - 交易时间每 30 秒刷新，非交易时间每 5 分钟
  - 解析返回数据更新标签
- 添加报警检测逻辑（待实现）

### 6. `waveshare-s3-rlcd-4.2.cc`（修改）
- BOOT 按钮：股票页时单击切换股票（`SwitchToNextStock()`）

---

## 三、已知问题和限制

### LVGL 控件兼容性（重要！）
- **lv_label**：正常工作 ✓
- **lv_line**：50 个点以上 heap corruption 崩溃 ✗
- **lv_chart**：RLCD 上完全不工作 ✗
- **lv_canvas**：PSRAM 缓冲区渲染异常 ✗
- **结论**：只能用 lv_label，不能用图形控件

### 内存限制
- LVGL 对象过多会导致 heap corruption
- 建议股票页对象总数不超过 15 个
- `font_puhui_16_4` 是验证可用的中文字体（`alibaba_puhui_16` 字符集不全）

---

## 四、待实现功能

### P0（必须）
1. **报警功能**
   - 喇叭提示音（BoxAudioCodec）
   - 个股：5分钟内涨跌超 ±3% 报警
   - 指数：10分钟内涨跌超 ±1% 报警
   - 报警方式：喇叭播放提示音 + 价格标签反色
   - 冷却机制：同一标的 10 分钟内不重复报警

2. **左侧列表滚动**
   - 超过 5 只股票时，BOOT 单击滚动列表
   - 当前选中项始终可见

### P1（后续）
3. NVS 持久化（股票列表存 Flash）
4. MCP 工具管理股票列表

---

## 五、按键映射

| 按键 | 操作 | 股票页行为 | 其他页行为 |
|------|------|-----------|-----------|
| BOOT 单击 | 切换股票 | 选中下一只 | 唤醒小智 |
| BOOT 双击 | 滚动列表 | 列表上下翻页 | 刷新数据 |
| USER 单击 | 切换页面 | 四页循环 | 同左 |

---

## 六、数据结构

```c
// 股票信息
struct StockInfo {
    const char* code;   // "sh600519"
    const char* name;   // "贵州茅台"
};
StockInfo stock_list_[10];      // 最多10只
int stock_list_count_ = 0;     // 当前数量
int stock_current_index_ = 0;  // 当前选中

// 指数（固定显示）
// 上证指数 sh000001
// 深证成指 sz399001
// 创业板指 sz399006
```

---

## 七、API 数据格式

### 腾讯财经 API
```
请求：http://qt.gtimg.cn/q=sh000001,sz399001,sh600519
返回：v_sh000001="1~上证指数~000001~3350.20~3322.66~...";
```

**字段说明（逗号分隔）**：
- [1] 名称
- [3] 当前价
- [4] 昨收
- [5] 今开
- [31] 涨跌额
- [32] 涨跌幅
- [33] 最高
- [34] 最低
- [37] 成交额（万元）

---

## 八、编译烧录

```bash
# 设置 ESP-IDF 环境
. C:\Espressif\tools\Microsoft.v5.5.4.PowerShell_profile.ps1

# 编译
idf.py build

# 烧录（需关闭占用 COM9 的程序）
idf.py -p COM9 flash

# 查看串口日志
idf.py -p COM9 monitor
```

---

## 九、测试要点

1. 切换到股票页（USER 按钮按 3 次）
2. 确认顶部显示上证/深证指数 + 实时数据
3. 确认左侧 5 只股票列表，每只显示实时价格
4. 确认 BOOT 单击切换股票，右侧详情同步更新
5. 确认不崩溃，内存充足
6. 确认状态栏时钟、WiFi、电池正常
