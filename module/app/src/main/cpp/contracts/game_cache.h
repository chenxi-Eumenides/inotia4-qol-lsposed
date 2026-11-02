#pragma once

// 游戏写操作成功后刷新缓存，使后续读取立即观察到最新状态。
void frame_cache_force_refresh();
