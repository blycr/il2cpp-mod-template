// user_hooks.cpp — 明日方舟战斗定制工作台（主线程安全 / 零 GC 停顿 / 防重幂等）+ 6 大内置分析工具
// ============================================================================
// 线程分工（严格遵守 Unity 单线程铁律，彻底杜绝跨线程崩溃）：
//   · Mod_DrawPanelTabs()  渲染线程 (Present)      -> 只画 ImGui + 写请求槽 PostRequestEx()
//   · Mod_OnMainPump()     Unity 主线程顶层消息泵 (ExitWndProc 收到 WM_APP+0x4D4F)
//                          -> 全部战斗状态守卫、修改指令、持续锁定、刷怪、调速、零 GC 快照
//   · Mod_OnTick()         Worker 线程 (每 100ms)  -> 仅跑纯后台非 Unity 任务 (类 Dump / Watch 缓存 / 配置落盘)
// ============================================================================
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>

#include <imgui.h>

#include "il2cpp/il2cpp_runtime.h"
#include "hook/il2cpp_hook.h"
#include "mod_config.h"
#include "probe.h"
#include "ui/ui_api.h"
#include "core/log.h"
#include "core/config.h"
#include "core/guard.h"
#include "core/hotkey.h"
#include "core/reflect.h"
#include "core/taskbus.h"
#include "core/gcobj.h"
#include "panels/panels.h"

namespace ir = il2cpp::runtime;

extern void Log(const char *fmt, ...);   // main.cpp
extern "C" ULONG_PTR ModSelfBase();      // hot_reload.cpp

namespace
{
    // 极简自旋锁（零 STL / 零系统句柄 / 手动映射 100% 安全）
    inline void SpinLock(volatile LONG *lk)
    {
        while (InterlockedCompareExchange(lk, 1, 0) != 0)
            Sleep(0);
    }
    inline void SpinUnlock(volatile LONG *lk)
    {
        InterlockedExchange(lk, 0);
    }

    enum FeedbackLevel
    {
        FB_INFO = 0,
        FB_OK   = 1,
        FB_WARN = 2,
        FB_ERR  = 3,
    };

    struct FeedbackEntry
    {
        int  level;
        char timeStr[16];
        char msg[320];
    };

    enum BattlePhase
    {
        PHASE_LOBBY    = 0,  // 主界面 / 登录 / 选关（无 BattleController）
        PHASE_LOADING  = 1,  // 关卡初始化中（m_state = NONE / INITED_BUT_NOT_START）
        PHASE_PLAYING  = 2,  // 战斗进行中（m_state = PLAYING，唯一合法修改窗口）
        PHASE_FINISHED = 3,  // 战斗已结算（m_state = FINISHED，禁止修改）
    };

    enum ReqKind
    {
        REQ_NONE = 0,
        REQ_DUMP_CLASS,
        REQ_SCAN_ROSTER,          // 立即刷新本局编队干员 & 怪物快照
        REQ_RESET_SESSION,        // 一键复位本局状态（关全锁 + 还原规则 + 选全体 + 恢复 1x 速度 + 刷快照）
        REQ_MOD_CARD_COST,        // 手牌 0 费部署 + 刷新 UI
        REQ_MOD_CARD_RESPAWN,     // 秒清手牌再部署冷却
        REQ_MOD_DEPLOY_LIMIT,     // 解除部署人数上限（手牌不占部署数 + 战场上调至 99）
        REQ_MOD_CHAR_ATTR,        // 修改场上我方干员指定属性
        REQ_MOD_CHAR_FULL_HPSP,   // 场上我方干员满血 + 满技力
        REQ_MOD_CHAR_AMMO,        // 修改场上弹药类干员弹药数量 / 弹药上限
        REQ_MOD_ANGEL_PRESET,     // 一键超载机枪模式
        REQ_MOD_BATTLE_DP,        // 战场当前总费用 DP = 99 & 上限 999
        REQ_MOD_BATTLE_LIFE,      // 战场蓝门生命值 = 999
        REQ_MOD_BATTLE_SPEED,     // 修改战斗倍速 (Unity 主线程)
        REQ_MOD_ENEMY_ATTR,       // 修改场上敌方怪物指定属性
        REQ_MOD_ENEMY_SANDBAG,    // 场上怪物改造成百万血零攻木桩沙包
        REQ_MOD_ENEMY_UNHARMFUL,  // 场上怪物进蓝门不扣血
        REQ_MOD_ENEMY_KILL,       // 一键秒杀场上匹配怪物
        REQ_SCAN_STAGE_ENEMIES,   // 读取本关可用怪物 ID 列表
        REQ_SPAWN_ENEMY,          // 在本关动态刷出指定怪物（主线程生成）
        // ---- 底层战斗规则粉碎机 & 进阶整活黑科技 ----
        REQ_TOGGLE_ALL_TERRAIN,   // 全地形违章放置（高台/地面/禁区均可放置，自动避开红门/蓝门/传送门）
        REQ_TOGGLE_OVERLAP,       // 同格无限叠放干员（Tile.enableOverlap）
        REQ_TOGGLE_GLOBAL_RANGE,  // 全地图攻击范围（挂载原生 GlobalRange 组件）
        REQ_TOGGLE_IGNORE_MOTION, // 无视对地/对空判定 + 破隐匿/迷彩
        REQ_TOGGLE_IMMUNE,        // 泰拉霸体绝缘体（高抵抗 + 免元素损伤 + 持续清控）
        REQ_MOD_CHAR_TRUE_DMG,    // 因果律真伤（物/法必中 + 100% 双穿）
        REQ_MOD_CHAR_MEGA_FORCE,  // 大力出奇迹（干员基础推拉力度 = 10）
        REQ_MOD_CHAR_REFUND,      // 撤退 100% 返还费用
        REQ_MOD_ENEMY_WEIGHTLESS, // 怪物零重量（MASS_LEVEL = 0）
        REQ_MOD_ENEMY_REVEAL,     // 照妖镜：立刻清除场上怪物隐匿/迷彩状态
    };

    struct Request
    {
        volatile LONG kind;
        char          text[128];
        int           attrType;
        float         val;
        int           extraInt1;
        int           extraInt2;
    };

    // ---- 本局带入干员实时快照（Worker 扫描 -> UI 零误输点选 + 实时面板显示）----
    struct StageCharInfo
    {
        char  id[96];
        char  nameCn[96];
        bool  inHand;
        bool  deployed;
        int   cost;
        float hp;
        float maxHp;
        float atk;
        float def;
        float magicRes;
        float atkSpd;
        float baseAtkTime;
        float blockCnt;
        float sp;
        float maxSp;
        bool  hasAmmo;
        int   ammoRem;
        int   ammoMax;
    };

    // ---- 场上存活怪物汇总快照 ----
    struct LiveEnemyGroup
    {
        char  key[96];
        char  name[96];
        int   count;
        float hp;
        float maxHp;
        float atk;
        float def;
        float moveSpd;
    };

    // ---- 本关预加载的可用怪物列表缓存 ----
    struct StageEnemyInfo
    {
        char id[96];
        char name[96];
    };

    // 本局已生效状态追踪（切关/重开/结算/复位自动清零；用于按钮变色高亮 + 防止重复叠加修改）
    struct BattleAppliedState
    {
        bool     zeroCost;
        bool     zeroRespawn;
        bool     deployLimit;
        bool     life999;
        bool     overload;
        bool     ammoBoosted;
        bool     trueDmg;
        bool     megaForce;
        bool     fullRefund;
        bool     enemySandbag;
        bool     enemyFrozen;
        bool     enemyUnharmful;
        bool     enemyWeightless;
        int      speedMode;        // 0 = 原生档位, 1 = 自定义 timeScale
        int      speedLevel;       // 0=0.5x, 1=1x, 2=2x, 3=极速
        float    timeScale;
        uint64_t charAttrMask;     // bit (attrType) = 本局已改过该干员属性 (支持 0..63)
        uint64_t enemyAttrMask;    // bit (attrType) = 本局已改过该怪物属性
    };

    // ---- 规则改写可逆备份缓存（纯 POD 静态数组，零堆分配/零 STL，切关自动清零，关闭时精确还原）----
    struct TileBackup
    {
        Il2CppObject *tile;
        Il2CppObject *tileData;
        int32_t       origBuildableType;
        int32_t       origAdvMask;
        uint8_t       origBlockManual;
        uint8_t       origOverlap;
        uint8_t       origOnlyManualOverlap;
    };

    struct CardCondBackup
    {
        Il2CppObject *card;
        int32_t       origBuildType;
        int32_t       origOrigBuildType;
        int32_t       origAdvMask;
        uint8_t       origCheckManual;
        int32_t       origManualType;
        uint8_t       origExcludeWalk;
        uint8_t       origExcludeNoTarget;
    };

    struct RangeBackup
    {
        Il2CppObject *selector;
        Il2CppObject *origRange;
    };

    struct MotionBackup
    {
        Il2CppObject *selector;
        int32_t       origOptMotion;
        uint8_t       origOptIgnoreMotion;
        uint8_t       origOptAdv;
        uint8_t       hasAdvFields;
        int32_t       origAdvMotion;
        uint8_t       origAdvIgnoreMotion;
        uint8_t       origAdvForceIgnoreCamo;
    };

    TileBackup     g_tileBackups[512];
    int            g_tileBackupCount     = 0;
    CardCondBackup g_cardCondBackups[128];
    int            g_cardCondBackupCount = 0;
    RangeBackup    g_rangeBackups[256];
    int            g_rangeBackupCount    = 0;
    MotionBackup   g_motionBackups[256];
    int            g_motionBackupCount   = 0;

    Request       g_req;
    volatile LONG g_dumpPending  = 0;
    char          g_dumpText[128]= { 0 };
    volatile LONG g_reqSeq       = 0;
    volatile LONG g_reqDone      = 0;
    volatile LONG g_inMainPump   = 0;
    ULONGLONG     g_lastPumpTick = 0;
    unsigned      g_tickCounter  = 0;
    unsigned      g_mainTick     = 0;
    volatile LONG g_inBattle     = 0;
    volatile LONG g_battlePhase  = PHASE_LOBBY;
    volatile LONG g_isAutoReplay = 0;
    volatile LONG g_battleSessId = 0;
    volatile LONG g_autoResetNew = 1;       // 默认开启：切关 / 重开 / 代理指挥自动清锁与旧目标，防脏状态
    Il2CppObject *g_lastBcPtr    = nullptr;
    uint32_t      g_lastFrameCnt = 0;
    char          g_curLevelId[96] = { 0 };
    volatile LONG g_snapLock     = 0;
    volatile LONG g_fbLock       = 0;

    BattleAppliedState g_applied = { false, false, false, false, false, false, false, false, false, false, false, false, false, 0, 1, 1.0f, 0, 0 };

    // 反馈横幅 & 最近 8 条操作记录
    FeedbackEntry g_lastFeedback = { FB_INFO, "", "黑科技终端已就位：随便进个关卡，立刻接管泰拉物理法则！" };
    FeedbackEntry g_fbHist[8];
    int           g_fbHead       = 0;
    int           g_fbCount      = 0;

    // 自动扫描快照
    StageCharInfo  g_stageChars[32];
    int            g_stageCharCount  = 0;
    LiveEnemyGroup g_liveEnemies[32];
    int            g_liveEnemyCount  = 0;
    int            g_liveEnemyTotal  = 0;
    StageEnemyInfo g_stageEnemies[64];
    volatile LONG  g_stageEnemyCount = 0;
    volatile LONG  g_stageRouteCount = 0;

    // ---- 1. 我方干员选择与定制 UI 状态 ----
    char  g_selectedCharId[128]   = "";      // 精确目标干员 ID（空 = 全体本局干员；点击干员按钮自动填入，零拼写错误）
    char  g_selectedCharLabel[128]= "全体罗德岛干员";
    char  g_charSearch[128]       = "";      // 中文/英文模糊筛选词（支持中文输入法直接输入）
    int   g_cardCostDelta         = -99;
    float g_charAtkSpeed          = 500.0f;
    float g_charBaseAtkTime       = 0.12f;
    float g_charAtk               = 5000.0f;
    float g_charMaxHp             = 50000.0f;
    float g_charDef               = 2000.0f;
    float g_charMagicRes          = 80.0f;
    float g_charBlockCnt          = 5.0f;
    float g_charSpRecov           = 30.0f;
    float g_charTaunt             = 10.0f;
    float g_charForce             = 10.0f;
    int   g_charAmmoTarget        = 99;

    // 持续自动锁定与底层规则改写开关（100% 在 Unity 主线程 Mod_OnMainPump 维护）
    volatile LONG g_lockInfAmmo      = 0;
    volatile LONG g_lockInfSp        = 0;
    volatile LONG g_lockGodHp        = 0;
    volatile LONG g_lockBattleDp     = 0;
    volatile LONG g_lockImmune       = 0;
    volatile LONG g_ruleAllTerrain   = 0;
    volatile LONG g_ruleOverlap      = 0;
    volatile LONG g_ruleGlobalRange  = 0;
    volatile LONG g_ruleIgnoreMotion = 0;

    // ---- 2. 敌方怪物定制 & 刷怪 UI 状态 ----
    char  g_enemyFilter[128]      = "";      // 空 = 场上全体敌人；点选上方怪物按钮或输入中文/ID 筛选
    char  g_enemyLabel[128]       = "全场所有倒霉蛋";
    float g_enemyMaxHp            = 1000000.0f;
    float g_enemyAtk              = 0.0f;
    float g_enemyDef              = 0.0f;
    float g_enemyMagicRes         = 0.0f;
    float g_enemyMoveSpd          = 0.0f;
    float g_enemyAtkSpd           = 100.0f;

    char  g_spawnEnemyKey[128]    = "";
    int   g_spawnRouteIdx         = 0;
    int   g_spawnCount            = 1;
    bool  g_spawnApplyCustom      = true;

    // ---- 3. 全局战场倍速 UI 状态 ----
    float g_customTimeScale       = 3.0f;
}

static void CopyStrSafe(char *dst, int cap, const char *src)
{
    if (!dst || cap <= 0) return;
    _snprintf_s(dst, cap, _TRUNCATE, "%s", src ? src : "");
}

static void SetFeedback(int level, const char *fmt, ...)
{
    char msg[320];
    va_list ap;
    va_start(ap, fmt);
    _vsnprintf_s(msg, sizeof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    char tbuf[16];
    _snprintf_s(tbuf, sizeof(tbuf), _TRUNCATE, "%02u:%02u:%02u",
                (unsigned)st.wHour, (unsigned)st.wMinute, (unsigned)st.wSecond);

    SpinLock(&g_fbLock);
    g_lastFeedback.level = level;
    CopyStrSafe(g_lastFeedback.timeStr, sizeof(g_lastFeedback.timeStr), tbuf);
    CopyStrSafe(g_lastFeedback.msg, sizeof(g_lastFeedback.msg), msg);

    g_fbHist[g_fbHead] = g_lastFeedback;
    g_fbHead = (g_fbHead + 1) & 7;
    if (g_fbCount < 8) ++g_fbCount;
    SpinUnlock(&g_fbLock);

    const char *tag = (level == FB_OK) ? "OK" : (level == FB_WARN) ? "WARN" : (level == FB_ERR) ? "ERR" : "INFO";
    Log("[user][%s] %s", tag, msg);
}

static void PostRequestEx(ReqKind kind, const char *text, int attrType, float val, int ex1 = 0, int ex2 = 0)
{
    if (kind == REQ_DUMP_CLASS)
    {
        CopyStrSafe(g_dumpText, sizeof(g_dumpText), text);
        InterlockedExchange(&g_dumpPending, 1);
        InterlockedIncrement(&g_reqSeq);
        return;
    }
    CopyStrSafe(g_req.text, sizeof(g_req.text), text);
    g_req.attrType  = attrType;
    g_req.val       = val;
    g_req.extraInt1 = ex1;
    g_req.extraInt2 = ex2;
    InterlockedExchange(&g_req.kind, (LONG)kind);
    InterlockedIncrement(&g_reqSeq);
    // 立即唤醒 Unity 主线程消息泵，0ms 响应按钮点击
    if (HWND hwnd = UiGameHwnd())
        PostMessageW(hwnd, WM_APP + 0x4D4F, 0, 0);
}

static void Il2CppStrToUtf8(Il2CppObject *strObj, char *out, int cap)
{
    if (!out || cap <= 0) return;
    out[0] = 0;
    if (!strObj) return;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_string_length || !api.il2cpp_string_chars) return;
    int32_t len = api.il2cpp_string_length((Il2CppString *)strObj);
    const Il2CppChar *chars = api.il2cpp_string_chars((Il2CppString *)strObj);
    if (!chars || len <= 0) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, (const wchar_t *)chars, len, out, cap - 1, nullptr, nullptr);
    out[n > 0 ? n : 0] = 0;
}

static bool MatchSubstringIgnoreCase(const char *hay, const char *needle)
{
    if (!needle || !needle[0]) return true;
    if (!hay || !hay[0]) return false;
    for (const char *p = hay; *p; ++p)
    {
        const char *h = p;
        const char *n = needle;
        while (*h && *n)
        {
            char ch1 = (*h >= 'A' && *h <= 'Z') ? (*h + 32) : *h;
            char ch2 = (*n >= 'A' && *n <= 'Z') ? (*n + 32) : *n;
            if (ch1 != ch2) break;
            ++h; ++n;
        }
        if (!*n) return true;
    }
    return false;
}

// 读取 ObscuredFP 槽位 (40 字节：key@+0, hidden@+8, inited@+16, fake@+24, fakeActive@+32)
static float ReadObscuredFpSlot(const void *slot)
{
    if (!slot) return 0.0f;
    const int64_t *p = (const int64_t *)slot;
    int64_t raw = p[0] ? (p[1] ^ p[0]) : p[1];
    return (float)((double)raw / 4294967296.0);
}

// 写入 ObscuredFP 槽位
static void WriteObscuredFpSlot(void *slot, int64_t fpRaw)
{
    if (!slot) return;
    int64_t key = *(int64_t *)slot;
    *((int64_t *)slot + 1) = fpRaw ^ key;
    *((int64_t *)slot + 3) = fpRaw;
}

// 读取 Attributes 对象的指定属性当前值
static float ReadAttributeOnObj(Il2CppObject *attrObj, int32_t attrType)
{
    if (!attrObj || attrType < 0 || attrType >= 38) return 0.0f;
    Il2CppApi &api = ir::Api();
    Il2CppClass *ak = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(attrObj) : attrObj->klass;
    if (!ak) return 0.0f;
    const char *arrFields[2] = { "m_cachedData", "m_rawData" };
    for (int a = 0; a < 2; ++a)
    {
        if (FieldInfo *fArr = ArkReflectFindField(ak, arrFields[a], nullptr))
        {
            if (Il2CppObject *arr = api.il2cpp_field_get_value_object(fArr, attrObj))
            {
                const int32_t len = *(int32_t *)((char *)arr + 0x18);
                if (attrType < len)
                    return ReadObscuredFpSlot((char *)arr + 0x20 + (size_t)attrType * 40);
            }
        }
    }
    return 0.0f;
}

// 写入 Attributes 对象指定属性：拓宽 m_dataRange + 写 m_rawData/m_cachedData + OverwriteRawData
static bool OverwriteAttributeOnObj(Il2CppObject *attrObj, int32_t attrType, float val)
{
    if (!attrObj || attrType < 0 || attrType >= 38) return false;
    Il2CppApi &api = ir::Api();
    Il2CppClass *ak = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(attrObj) : attrObj->klass;
    if (!ak) return false;

    const int64_t fpRaw = (int64_t)((double)val * 4294967296.0);

    if (FieldInfo *fRange = ArkReflectFindField(ak, "m_dataRange", nullptr))
    {
        Il2CppObject *rangeArr = api.il2cpp_field_get_value_object(fRange, attrObj);
        if (rangeArr)
        {
            const int32_t len = *(int32_t *)((char *)rangeArr + 0x18);
            if (attrType < len)
            {
                int64_t *pair = (int64_t *)((char *)rangeArr + 0x20 + (size_t)attrType * 16);
                if (fpRaw < pair[0]) pair[0] = fpRaw;
                if (fpRaw > pair[1]) pair[1] = fpRaw;
            }
        }
    }

    const char *arrFields[2] = { "m_rawData", "m_cachedData" };
    for (int a = 0; a < 2; ++a)
    {
        if (FieldInfo *fArr = ArkReflectFindField(ak, arrFields[a], nullptr))
        {
            Il2CppObject *arr = api.il2cpp_field_get_value_object(fArr, attrObj);
            if (arr)
            {
                const int32_t len = *(int32_t *)((char *)arr + 0x18);
                if (attrType < len)
                {
                    void *slot = (char *)arr + 0x20 + (size_t)attrType * 40;
                    WriteObscuredFpSlot(slot, fpRaw);
                }
            }
        }
    }

    if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
    {
        if (MethodInfo *mOw = api.il2cpp_class_get_method_from_name(ak, "OverwriteRawData", 3))
        {
            uint8_t refresh = 1;
            void *args[3] = { &attrType, (void *)&fpRaw, &refresh };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mOw, attrObj, args, &exc);
        }
    }
    return true;
}

// 检查 Entity 是否仍在场上存活
static bool IsEntityAlive(Il2CppObject *entityObj, Il2CppClass *ek)
{
    if (!entityObj || !ek) return false;
    Il2CppApi &api = ir::Api();
    if (FieldInfo *fFinish = ArkReflectFindField(ek, "<finishReason>k__BackingField", nullptr))
    {
        if (void *addr = ArkReflectFieldAddr(fFinish, entityObj))
        {
            if (*(int32_t *)addr != 0)
                return false;
        }
    }
    if (FieldInfo *fHp = ArkReflectFindField(ek, "m_hp", nullptr))
    {
        if (void *addr = ArkReflectFieldAddr(fHp, entityObj))
        {
            if (*(int64_t *)addr <= 0)
                return false;
        }
    }
    if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
    {
        Il2CppClass *cur = ek;
        MethodInfo *mAlive = nullptr;
        while (cur && !mAlive)
        {
            mAlive = api.il2cpp_class_get_method_from_name(cur, "get_isAlive", 0);
            cur = api.il2cpp_class_get_parent(cur);
        }
        if (mAlive)
        {
            Il2CppException *exc = nullptr;
            Il2CppObject *ret = api.il2cpp_runtime_invoke(mAlive, entityObj, nullptr, &exc);
            if (!exc && ret && api.il2cpp_object_unbox)
            {
                uint8_t *b = (uint8_t *)api.il2cpp_object_unbox(ret);
                if (b && !*b) return false;
            }
        }
    }
    return true;
}

static bool ExtractCharIdName(Il2CppObject *dataObj, char *idOut, int idCap, char *nameOut, int nameCap)
{
    if (idOut && idCap > 0) idOut[0] = 0;
    if (nameOut && nameCap > 0) nameOut[0] = 0;
    if (!dataObj) return false;
    Il2CppApi &api = ir::Api();
    Il2CppClass *dk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(dataObj) : dataObj->klass;
    if (!dk) return false;
    if (idOut && idCap > 0)
    {
        if (FieldInfo *fId = ArkReflectFindField(dk, "id", nullptr))
            Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fId, dataObj), idOut, idCap);
    }
    if (nameOut && nameCap > 0)
    {
        if (FieldInfo *fName = ArkReflectFindField(dk, "nameCn", nullptr))
            Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fName, dataObj), nameOut, nameCap);
    }
    return (idOut && idOut[0]) || (nameOut && nameOut[0]);
}

static bool CheckCharDataMatches(Il2CppObject *dataObj, const char *filter, char *outDesc, int descCap)
{
    if (outDesc && descCap > 0) outDesc[0] = 0;
    char idBuf[96] = { 0 };
    char nameBuf[96] = { 0 };
    if (!ExtractCharIdName(dataObj, idBuf, sizeof(idBuf), nameBuf, sizeof(nameBuf)))
        return false;

    if (idBuf[0] && strncmp(idBuf, "char_", 5) != 0 && strncmp(idBuf, "token_", 6) != 0 && (!filter || !filter[0]))
        return false;

    if (outDesc && descCap > 0)
        _snprintf_s(outDesc, descCap, _TRUNCATE, "%s(%s)", nameBuf[0] ? nameBuf : "?", idBuf[0] ? idBuf : "?");

    if (!filter || !filter[0])
        return true;
    return MatchSubstringIgnoreCase(idBuf, filter) || MatchSubstringIgnoreCase(nameBuf, filter);
}

static bool ExtractEnemyKeyName(Il2CppObject *enemyObj, Il2CppClass *ek, char *keyOut, int keyCap, char *nameOut, int nameCap)
{
    if (keyOut && keyCap > 0) keyOut[0] = 0;
    if (nameOut && nameCap > 0) nameOut[0] = 0;
    if (!enemyObj || !ek) return false;
    Il2CppApi &api = ir::Api();

    if (keyOut && keyCap > 0)
    {
        if (FieldInfo *fId = ArkReflectFindField(ek, "<id>k__BackingField", nullptr))
            Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fId, enemyObj), keyOut, keyCap);
    }
    if (FieldInfo *fData = ArkReflectFindField(ek, "<data>k__BackingField", nullptr))
    {
        if (Il2CppObject *dataObj = api.il2cpp_field_get_value_object(fData, enemyObj))
        {
            Il2CppClass *dk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(dataObj) : dataObj->klass;
            if (dk)
            {
                if (keyOut && keyCap > 0 && !keyOut[0])
                {
                    if (FieldInfo *fKey = ArkReflectFindField(dk, "key", nullptr))
                        Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fKey, dataObj), keyOut, keyCap);
                }
                if (nameOut && nameCap > 0)
                {
                    if (FieldInfo *fName = ArkReflectFindField(dk, "name", nullptr))
                        Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fName, dataObj), nameOut, nameCap);
                }
            }
        }
    }
    return (keyOut && keyOut[0]) || (nameOut && nameOut[0]);
}

static bool CheckEnemyMatches(Il2CppObject *enemyObj, Il2CppClass *ek, const char *filter, char *outDesc, int descCap)
{
    if (outDesc && descCap > 0) outDesc[0] = 0;
    char keyBuf[96] = { 0 };
    char nameBuf[96] = { 0 };
    if (!ExtractEnemyKeyName(enemyObj, ek, keyBuf, sizeof(keyBuf), nameBuf, sizeof(nameBuf)))
        return false;
    if (outDesc && descCap > 0)
        _snprintf_s(outDesc, descCap, _TRUNCATE, "%s(%s)", nameBuf[0] ? nameBuf : "敌人", keyBuf[0] ? keyBuf : "?");
    if (!filter || !filter[0])
        return true;
    return MatchSubstringIgnoreCase(keyBuf, filter) || MatchSubstringIgnoreCase(nameBuf, filter);
}

static void SetEntityCurrentHp(Il2CppObject *entityObj, Il2CppClass *ek, float val)
{
    if (!entityObj || !ek) return;
    Il2CppApi &api = ir::Api();
    int64_t fpRaw = (int64_t)((double)val * 4294967296.0);
    if (FieldInfo *fHp = ArkReflectFindField(ek, "m_hp", nullptr))
    {
        if (void *addr = ArkReflectFieldAddr(fHp, entityObj))
            *(int64_t *)addr = fpRaw;
    }
    if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
    {
        if (MethodInfo *mSetHp = api.il2cpp_class_get_method_from_name(ek, "SetHpDirectly", 1))
        {
            void *args[1] = { &fpRaw };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetHp, entityObj, args, &exc);
        }
    }
}

static void FillEntitySp(Il2CppObject *entityObj, Il2CppClass *ek)
{
    if (!entityObj || !ek) return;
    int32_t maxSp = 100;
    if (FieldInfo *fMaxSp = ArkReflectFindField(ek, "<maxSp>k__BackingField", nullptr))
    {
        if (void *addr = ArkReflectFieldAddr(fMaxSp, entityObj))
            maxSp = *(int32_t *)addr;
    }
    if (maxSp <= 0) maxSp = 100;
    if (FieldInfo *fSp = ArkReflectFindField(ek, "m_sp", nullptr))
    {
        if (void *addr = ArkReflectFieldAddr(fSp, entityObj))
            WriteObscuredFpSlot(addr, (int64_t)maxSp * 4294967296LL);
    }
}

// ============================================================================
// 弹药 / 子弹引擎 (CastSkillWithLimitTimes + AbilityEventCounter)
// ============================================================================
static bool IsCounterValid(Il2CppObject *counter, int32_t *outRem, int32_t *outMax)
{
    if (!counter) return false;
    Il2CppApi &api = ir::Api();
    Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(counter) : counter->klass;
    if (!ck) return false;

    FieldInfo *fEvt = ArkReflectFindField(ck, "m_eventCount", nullptr);
    FieldInfo *fMax = ArkReflectFindField(ck, "m_maxCount", nullptr);
    if (!fEvt || !fMax) return false;

    int32_t evt = 0, mx = 0;
    if (void *a = ArkReflectFieldAddr(fEvt, counter)) evt = *(int32_t *)a;
    if (void *a = ArkReflectFieldAddr(fMax, counter)) mx  = *(int32_t *)a;
    if (mx <= 0) return false;

    int32_t rem = mx - evt;
    if (rem < 0) rem = 0;
    if (outRem) *outRem = rem;
    if (outMax) *outMax = mx;
    return true;
}

// 收集干员身上全部有效的 AbilityEventCounter（覆盖常规弹药技、过载弹药技、多模式弹药技）
static int FindCharAmmoCounters(Il2CppObject *chr, Il2CppClass *ck, Il2CppObject **outList, int maxOut,
                                Il2CppObject **allCounters, int allCounterCount)
{
    if (!chr || !ck || !outList || maxOut <= 0) return 0;
    Il2CppApi &api = ir::Api();
    int found = 0;

    auto addUnique = [&](Il2CppObject *c) {
        if (!c || found >= maxOut) return;
        for (int i = 0; i < found; ++i)
            if (outList[i] == c) return;
        if (IsCounterValid(c, nullptr, nullptr))
            outList[found++] = c;
    };

    // 路径 1：Character.m_skill (CastSkillWithLimitTimes) -> m_progressSource / get_currentProgressSource()
    if (FieldInfo *fSkill = ArkReflectFindField(ck, "m_skill", nullptr))
    {
        if (Il2CppObject *skill = api.il2cpp_field_get_value_object(fSkill, chr))
        {
            Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(skill) : skill->klass;
            if (sk)
            {
                if (FieldInfo *fProg = ArkReflectFindField(sk, "m_progressSource", nullptr))
                    addUnique(api.il2cpp_field_get_value_object(fProg, skill));
                if (FieldInfo *fSerProg = ArkReflectFindField(sk, "_progressSource", nullptr))
                    addUnique(api.il2cpp_field_get_value_object(fSerProg, skill));
                if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
                {
                    Il2CppClass *cur = sk;
                    MethodInfo *mGetSrc = nullptr;
                    while (cur && !mGetSrc)
                    {
                        mGetSrc = api.il2cpp_class_get_method_from_name(cur, "get_currentProgressSource", 0);
                        cur = api.il2cpp_class_get_parent(cur);
                    }
                    if (mGetSrc)
                    {
                        Il2CppException *exc = nullptr;
                        Il2CppObject *c = api.il2cpp_runtime_invoke(mGetSrc, skill, nullptr, &exc);
                        if (!exc && c) addUnique(c);
                    }
                }
            }
        }
    }

    // 路径 2（零 GC 停顿）：遍历干员自身的 Entity.m_cachedAbilities (List<Ability>)，
    // 直接检查能力对象或其内部字段（m_eventCounter/_eventCounter/m_progressSource 等），
    // 无需全堆 STW 扫描即可捕获特性/天赋/过载弹药计数器。
    if (FieldInfo *fCachedAb = ArkReflectFindField(ck, "m_cachedAbilities", nullptr))
    {
        if (Il2CppObject *abList = api.il2cpp_field_get_value_object(fCachedAb, chr))
        {
            Il2CppClass *lk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(abList) : abList->klass;
            if (lk)
            {
                FieldInfo *fItems = ArkReflectFindField(lk, "_items", nullptr);
                FieldInfo *fSize  = ArkReflectFindField(lk, "_size", nullptr);
                if (fItems && fSize)
                {
                    int32_t sz = 0;
                    if (void *a = ArkReflectFieldAddr(fSize, abList)) sz = *(int32_t *)a;
                    if (sz > 0 && sz <= 256)
                    {
                        if (Il2CppObject *arr = api.il2cpp_field_get_value_object(fItems, abList))
                        {
                            int32_t arrLen = *(int32_t *)((char *)arr + 0x18);
                            int32_t take = (sz < arrLen) ? sz : arrLen;
                            if (take > 256) take = 256;
                            Il2CppObject **abItems = (Il2CppObject **)((char *)arr + 0x20);
                            static const char *kCounterFields[] = {
                                "m_progressSource", "_progressSource",
                                "m_eventCounter", "_eventCounter", "m_counter"
                            };
                            for (int i = 0; i < take && found < maxOut; ++i)
                            {
                                Il2CppObject *ab = abItems[i];
                                if (!ab) continue;
                                addUnique(ab);
                                Il2CppClass *ak = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(ab) : ab->klass;
                                if (!ak) continue;
                                for (int fi = 0; fi < 5 && found < maxOut; ++fi)
                                {
                                    if (FieldInfo *fc = ArkReflectFindField(ak, kCounterFields[fi], nullptr))
                                        addUnique(api.il2cpp_field_get_value_object(fc, ab));
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // 路径 3（仅手动按钮按需兜底，后台刷新/持续锁定绝不传入）：扫描传入的 AbilityEventCounter 列表
    if (allCounters && allCounterCount > 0 && api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
    {
        for (int i = 0; i < allCounterCount && found < maxOut; ++i)
        {
            Il2CppObject *c = allCounters[i];
            if (!c) continue;
            Il2CppClass *cc = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(c) : c->klass;
            if (!cc) continue;
            Il2CppClass *cur = cc;
            MethodInfo *mOwner = nullptr;
            while (cur && !mOwner)
            {
                mOwner = api.il2cpp_class_get_method_from_name(cur, "get_owner", 0);
                cur = api.il2cpp_class_get_parent(cur);
            }
            if (!mOwner) continue;
            Il2CppException *exc = nullptr;
            Il2CppObject *owner = api.il2cpp_runtime_invoke(mOwner, c, nullptr, &exc);
            if (!exc && owner == chr)
                addUnique(c);
        }
    }

    return found;
}

static bool ModifyAmmoCounter(Il2CppObject *counter, int32_t targetAmmo, bool changeMax,
                              int32_t *beforeRem, int32_t *beforeMax, int32_t *afterRem, int32_t *afterMax)
{
    if (!counter) return false;
    Il2CppApi &api = ir::Api();
    Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(counter) : counter->klass;
    if (!ck) return false;

    int32_t bRem = 0, bMax = 0;
    if (!IsCounterValid(counter, &bRem, &bMax)) return false;
    if (beforeRem) *beforeRem = bRem;
    if (beforeMax) *beforeMax = bMax;

    FieldInfo *fEvt     = ArkReflectFindField(ck, "m_eventCount", nullptr);
    FieldInfo *fMax     = ArkReflectFindField(ck, "m_maxCount", nullptr);
    FieldInfo *fOrigMax = ArkReflectFindField(ck, "m_originalMaxCount", nullptr);
    FieldInfo *fAddMax  = ArkReflectFindField(ck, "m_maxAdditionCount", nullptr);
    FieldInfo *fReady   = ArkReflectFindField(ck, "m_readyToReset", nullptr);

    if (changeMax && targetAmmo > 0)
    {
        int32_t origMax = bMax;
        if (fOrigMax)
        {
            if (void *a = ArkReflectFieldAddr(fOrigMax, counter))
                if (*(int32_t *)a > 0) origMax = *(int32_t *)a;
        }
        int32_t addCnt = targetAmmo - origMax;
        if (fAddMax)
        {
            if (void *a = ArkReflectFieldAddr(fAddMax, counter))
                *(int32_t *)a = addCnt;
        }
        if (fMax)
        {
            if (void *a = ArkReflectFieldAddr(fMax, counter))
                *(int32_t *)a = targetAmmo;
        }
        if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
        {
            if (MethodInfo *mSetAdd = api.il2cpp_class_get_method_from_name(ck, "SetMaxAdditionCount", 1))
            {
                void *args[1] = { &addCnt };
                Il2CppException *exc = nullptr;
                api.il2cpp_runtime_invoke(mSetAdd, counter, args, &exc);
            }
        }
    }

    if (fEvt)
    {
        if (void *a = ArkReflectFieldAddr(fEvt, counter))
            *(int32_t *)a = 0;
    }
    if (fReady)
    {
        if (void *a = ArkReflectFieldAddr(fReady, counter))
            *(uint8_t *)a = 0;
    }

    // 调用原生 RecoverEventCount 同步共享黑板与 HUD 子弹条
    if (api.il2cpp_class_get_method_from_name && api.il2cpp_runtime_invoke)
    {
        if (MethodInfo *mRec = api.il2cpp_class_get_method_from_name(ck, "RecoverEventCount", 2))
        {
            int32_t rec = (targetAmmo > 0) ? targetAmmo : bMax;
            uint8_t skipLim = 1;
            void *args[2] = { &rec, &skipLim };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mRec, counter, args, &exc);
        }
    }

    int32_t aRem = 0, aMax = 0;
    IsCounterValid(counter, &aRem, &aMax);
    if (afterRem) *afterRem = aRem;
    if (afterMax) *afterMax = aMax;
    return true;
}

static Il2CppObject *GetBattleControllerFast();
static int GetBattleCharactersFast(Il2CppObject *bc, Il2CppObject **out, int maxOut);

static int ApplyCharacterAmmo(const char *filter, int32_t targetAmmo, bool changeMax,
                              int *outCharMatched, char *lastMatched, int cap,
                              int32_t *outBRem, int32_t *outBMax, int32_t *outARem, int32_t *outAMax)
{
    Il2CppApi &api = ir::Api();
    if (outCharMatched) *outCharMatched = 0;

    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!ce || !ce->klass) return -1;

    Il2CppObject *chars[256];
    int n = bc ? GetBattleCharactersFast(bc, chars, 256) : -1;
    if (n < 0) n = ArkGcEnumObjects(ce->klass, chars, 256, 1);

    FieldInfo *fData = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    int modified = 0;
    int matchedChars = 0;

    auto processPass = [&](Il2CppObject **allCounters, int allCnt) {
        for (int i = 0; i < n; ++i)
        {
            Il2CppObject *chr = chars[i];
            if (!chr || !fData || !IsEntityAlive(chr, ce->klass)) continue;
            char desc[128] = { 0 };
            if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
                continue;
            if (!allCounters) ++matchedChars;

            Il2CppObject *myCnts[8];
            int k = FindCharAmmoCounters(chr, ce->klass, myCnts, 8, allCounters, allCnt);
            for (int j = 0; j < k; ++j)
            {
                int32_t bR = 0, bM = 0, aR = 0, aM = 0;
                if (ModifyAmmoCounter(myCnts[j], targetAmmo, changeMax, &bR, &bM, &aR, &aM))
                {
                    if (outBRem) *outBRem = bR;
                    if (outBMax) *outBMax = bM;
                    if (outARem) *outARem = aR;
                    if (outAMax) *outAMax = aM;
                    CopyStrSafe(lastMatched, cap, desc);
                    ++modified;
                }
            }
        }
    };

    // 第一遍：纯零 GC 停顿路径（m_skill + m_cachedAbilities）
    processPass(nullptr, 0);

    // 仅当手动点击且零 GC 路径未命中任何弹药计数器时，才做一次按需 GC 兜底扫描
    if (modified == 0 && matchedChars > 0)
    {
        Il2CppObject *allCounters[128];
        int allCnt = 0;
        if (const ArkClassEntry *cntCe = ArkReflectFindClass("Torappu.Battle.Abilities.AbilityEventCounter"))
        {
            if (cntCe->klass)
                allCnt = ArkGcEnumObjects(cntCe->klass, allCounters, 128, 1);
        }
        if (allCnt > 0)
            processPass(allCounters, allCnt);
    }

    if (outCharMatched) *outCharMatched = matchedChars;
    return modified;
}

// ============================================================================
// 自动扫描：本局编队干员 + 场上实时怪物 + 本关预加载怪物
// ============================================================================
// 零 GC 停顿快速获取当前 BattleController 单例（直接读 SingletonMonoBehaviour<BattleController>.s_instance）
static Il2CppObject *GetBattleControllerFast()
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *bcCe = ArkReflectFindClass("Torappu.Battle.BattleController");
    if (!bcCe || !bcCe->klass) return nullptr;

    if (api.il2cpp_field_static_get_value)
    {
        if (FieldInfo *fInst = ArkReflectFindField(bcCe->klass, "s_instance", nullptr))
        {
            Il2CppObject *inst = nullptr;
            api.il2cpp_field_static_get_value(fInst, &inst);
            return inst;
        }
    }
    return nullptr;
}

// 零 GC 停顿读取 UnorderedArray<T>（UnitManager.characters / enemies 的底层容器）
// 返回值：>= 0 为成功读取到的数量（含 0）；-1 表示字段结构不可用。
static int ReadUnorderedArrayObjects(Il2CppObject *uaObj, Il2CppObject **out, int maxOut, Il2CppClass *filterBase = nullptr)
{
    if (!uaObj || !out || maxOut <= 0) return -1;
    Il2CppApi &api = ir::Api();
    Il2CppClass *uk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(uaObj) : uaObj->klass;
    if (!uk) return -1;

    FieldInfo *fItems = ArkReflectFindField(uk, "m_items", nullptr);
    FieldInfo *fCnt   = ArkReflectFindField(uk, "<count>k__BackingField", nullptr);
    if (!fItems || !fCnt) return -1;

    int32_t cnt = 0;
    if (void *a = ArkReflectFieldAddr(fCnt, uaObj)) cnt = *(int32_t *)a;
    if (cnt <= 0) return 0;

    Il2CppObject *arr = api.il2cpp_field_get_value_object(fItems, uaObj);
    if (!arr) return 0;

    int32_t arrLen = *(int32_t *)((char *)arr + 0x18);
    if (arrLen <= 0 || arrLen > 8192) return 0;
    int32_t take = (cnt < arrLen) ? cnt : arrLen;
    Il2CppObject **items = (Il2CppObject **)((char *)arr + 0x20);

    int n = 0;
    for (int32_t i = 0; i < take && n < maxOut; ++i)
    {
        Il2CppObject *obj = items[i];
        if (!obj) continue;
        if (filterBase && api.il2cpp_class_is_assignable_from)
        {
            Il2CppClass *ok = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(obj) : obj->klass;
            if (!ok || !api.il2cpp_class_is_assignable_from(filterBase, ok))
                continue;
        }
        out[n++] = obj;
    }
    return n;
}

// 零 GC 停顿读取场上所有干员：BattleController.<unitManager> -> UnitManager.<characters>
static int GetBattleCharactersFast(Il2CppObject *bc, Il2CppObject **out, int maxOut)
{
    if (!bc || !out || maxOut <= 0) return -1;
    Il2CppApi &api = ir::Api();
    Il2CppClass *bk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bk) return -1;

    FieldInfo *fUm = ArkReflectFindField(bk, "<unitManager>k__BackingField", nullptr);
    if (!fUm) return -1;
    Il2CppObject *um = api.il2cpp_field_get_value_object(fUm, bc);
    if (!um) return -1;

    Il2CppClass *umk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(um) : um->klass;
    if (!umk) return -1;
    FieldInfo *fChars = ArkReflectFindField(umk, "<characters>k__BackingField", nullptr);
    if (!fChars) return -1;
    Il2CppObject *ua = api.il2cpp_field_get_value_object(fChars, um);
    const ArkClassEntry *charCe = ArkReflectFindClass("Torappu.Battle.Character");
    return ReadUnorderedArrayObjects(ua, out, maxOut, charCe ? charCe->klass : nullptr);
}

// 零 GC 停顿读取场上所有怪物：BattleController.<unitManager> -> UnitManager.<enemies>
static int GetBattleEnemiesFast(Il2CppObject *bc, Il2CppObject **out, int maxOut)
{
    if (!bc || !out || maxOut <= 0) return -1;
    Il2CppApi &api = ir::Api();
    Il2CppClass *bk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bk) return -1;

    FieldInfo *fUm = ArkReflectFindField(bk, "<unitManager>k__BackingField", nullptr);
    if (!fUm) return -1;
    Il2CppObject *um = api.il2cpp_field_get_value_object(fUm, bc);
    if (!um) return -1;

    Il2CppClass *umk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(um) : um->klass;
    if (!umk) return -1;
    FieldInfo *fEnms = ArkReflectFindField(umk, "<enemies>k__BackingField", nullptr);
    if (!fEnms) return -1;
    Il2CppObject *ua = api.il2cpp_field_get_value_object(fEnms, um);
    const ArkClassEntry *enmCe = ArkReflectFindClass("Torappu.Battle.Enemy");
    return ReadUnorderedArrayObjects(ua, out, maxOut, enmCe ? enmCe->klass : nullptr);
}

// 零 GC 停顿读取手牌编队：BattleController.m_deckDict (List<KeyValuePair<PlayerSide, Deck>>) -> Deck.m_cards
static int GetBattleCardsFast(Il2CppObject *bc, Il2CppObject **out, int maxOut)
{
    if (!bc || !out || maxOut <= 0) return -1;
    Il2CppApi &api = ir::Api();
    Il2CppClass *bk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bk) return -1;

    FieldInfo *fDeckDict = ArkReflectFindField(bk, "m_deckDict", nullptr);
    if (!fDeckDict) return -1;
    Il2CppObject *dictObj = api.il2cpp_field_get_value_object(fDeckDict, bc);
    if (!dictObj) return -1;

    Il2CppClass *dk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(dictObj) : dictObj->klass;
    if (!dk) return -1;

    FieldInfo *fItems = ArkReflectFindField(dk, "_items", nullptr);
    FieldInfo *fSize  = ArkReflectFindField(dk, "_size", nullptr);
    if (!fItems || !fSize) return -1;

    int32_t sz = 0;
    if (void *a = ArkReflectFieldAddr(fSize, dictObj)) sz = *(int32_t *)a;
    if (sz <= 0 || sz > 32) return 0;

    Il2CppObject *kvArr = api.il2cpp_field_get_value_object(fItems, dictObj);
    if (!kvArr) return -1;

    int32_t arrLen = *(int32_t *)((char *)kvArr + 0x18);
    if (arrLen <= 0 || arrLen > 128) return 0;
    int32_t take = (sz < arrLen) ? sz : arrLen;

    // 动态读取 KeyValuePair<PlayerSide, Deck> 的元素步长和 value 字段偏移（零硬编码假设）
    Il2CppClass *arrKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(kvArr) : kvArr->klass;
    int elemStride = 16;
    size_t valOffset = 8;
    if (arrKlass && api.il2cpp_array_element_size)
    {
        int s = api.il2cpp_array_element_size(arrKlass);
        if (s >= 8 && s <= 64) elemStride = s;
    }
    if (arrKlass && api.il2cpp_class_get_element_class && api.il2cpp_field_get_offset)
    {
        if (Il2CppClass *elemK = api.il2cpp_class_get_element_class(arrKlass))
        {
            if (FieldInfo *fVal = ArkReflectFindField(elemK, "value", nullptr))
            {
                size_t rawOff = api.il2cpp_field_get_offset(fVal);
                // 值类型字段偏移在 IL2CPP 元数据中含 0x10 字节装箱头，数组内裸值需减去 0x10
                if (rawOff >= 0x10 && (rawOff - 0x10) + sizeof(void *) <= (size_t)elemStride)
                    valOffset = rawOff - 0x10;
            }
        }
    }

    char *base = (char *)kvArr + 0x20;
    int totalCards = 0;
    for (int32_t i = 0; i < take && totalCards < maxOut; ++i)
    {
        Il2CppObject *deck = *(Il2CppObject **)(base + (size_t)i * (size_t)elemStride + valOffset);
        if (!deck) continue;
        Il2CppClass *deckK = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(deck) : deck->klass;
        if (!deckK) continue;

        FieldInfo *fCards = ArkReflectFindField(deckK, "m_cards", nullptr);
        if (!fCards) continue;
        Il2CppObject *cardArr = api.il2cpp_field_get_value_object(fCards, deck);
        if (!cardArr) continue;

        int32_t clen = *(int32_t *)((char *)cardArr + 0x18);
        if (clen <= 0 || clen > 256) continue;
        Il2CppObject **cItems = (Il2CppObject **)((char *)cardArr + 0x20);
        for (int32_t j = 0; j < clen && totalCards < maxOut; ++j)
        {
            Il2CppObject *c = cItems[j];
            if (!c) continue;
            bool dup = false;
            for (int k = 0; k < totalCards; ++k)
                if (out[k] == c) { dup = true; break; }
            if (!dup) out[totalCards++] = c;
        }
    }
    return totalCards;
}

// 零 GC 停顿读取 BattleController 当前阶段/结算结果/加载完结标志/代理指挥/关卡ID/逻辑帧
static void ReadBattleControllerMeta(Il2CppObject *bc, int32_t *outState, bool *outAutoReplay,
                                     char *outLevelId, int lvlCap, uint32_t *outFrameCnt,
                                     int32_t *outResult = nullptr, bool *outLoaded = nullptr)
{
    if (outState)      *outState = 0;
    if (outAutoReplay) *outAutoReplay = false;
    if (outLevelId && lvlCap > 0) outLevelId[0] = 0;
    if (outFrameCnt)   *outFrameCnt = 0;
    if (outResult)     *outResult = 0;
    if (outLoaded)     *outLoaded = true;
    if (!bc) return;

    Il2CppApi &api = ir::Api();
    Il2CppClass *bk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bk) return;

    if (outState)
    {
        if (FieldInfo *fSt = ArkReflectFindField(bk, "m_state", nullptr))
            if (void *a = ArkReflectFieldAddr(fSt, bc)) *outState = *(int32_t *)a;
    }
    if (outResult)
    {
        if (FieldInfo *fRes = ArkReflectFindField(bk, "m_result", nullptr))
            if (void *a = ArkReflectFieldAddr(fRes, bc)) *outResult = *(int32_t *)a;
    }
    if (outLoaded)
    {
        if (FieldInfo *fLd = ArkReflectFindField(bk, "m_haveGameLoaded", nullptr))
            if (void *a = ArkReflectFieldAddr(fLd, bc)) *outLoaded = (*(uint8_t *)a != 0);
    }
    if (outAutoReplay)
    {
        if (FieldInfo *fRep = ArkReflectFindField(bk, "m_isAutoReplayOn", nullptr))
            if (void *a = ArkReflectFieldAddr(fRep, bc)) *outAutoReplay = (*(uint8_t *)a != 0);
    }
    if (outLevelId && lvlCap > 0)
    {
        if (FieldInfo *fLvlId = ArkReflectFindField(bk, "<levelId>k__BackingField", nullptr))
            Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fLvlId, bc), outLevelId, lvlCap);
    }
    if (outFrameCnt && api.il2cpp_field_static_get_value)
    {
        if (FieldInfo *fFrm = ArkReflectFindField(bk, "s_fixedFrameCnt", nullptr))
        {
            uint32_t frm = 0;
            api.il2cpp_field_static_get_value(fFrm, &frm);
            *outFrameCnt = frm;
        }
    }
}

// 清理上一局残留状态（防脏目标/脏怪物ID/脏持续锁定/按钮高亮状态带入下一场对战；绝不碰正在销毁的游戏对象）
static void ResetPerBattleState(bool clearLocks)
{
    if (clearLocks)
    {
        InterlockedExchange(&g_lockInfAmmo, 0);
        InterlockedExchange(&g_lockInfSp, 0);
        InterlockedExchange(&g_lockGodHp, 0);
        InterlockedExchange(&g_lockBattleDp, 0);
        InterlockedExchange(&g_lockImmune, 0);
    }
    // 规则改写开关与对象指针备份强制随对局清空（绝不把旧场景指针带进下一关！）
    InterlockedExchange(&g_ruleAllTerrain, 0);
    InterlockedExchange(&g_ruleOverlap, 0);
    InterlockedExchange(&g_ruleGlobalRange, 0);
    InterlockedExchange(&g_ruleIgnoreMotion, 0);
    g_tileBackupCount     = 0;
    g_cardCondBackupCount = 0;
    g_rangeBackupCount    = 0;
    g_motionBackupCount   = 0;

    SpinLock(&g_snapLock);
    memset(&g_applied, 0, sizeof(g_applied));
    g_applied.speedMode  = 0;
    g_applied.speedLevel = 1;
    g_applied.timeScale  = 1.0f;
    g_selectedCharId[0] = 0;
    CopyStrSafe(g_selectedCharLabel, sizeof(g_selectedCharLabel), "全体罗德岛干员");
    g_charSearch[0] = 0;
    g_enemyFilter[0] = 0;
    CopyStrSafe(g_enemyLabel, sizeof(g_enemyLabel), "全场所有倒霉蛋");
    g_spawnEnemyKey[0] = 0;
    g_customTimeScale = 1.0f;
    g_stageCharCount = 0;
    g_liveEnemyCount = 0;
    g_liveEnemyTotal = 0;
    SpinUnlock(&g_snapLock);
    InterlockedExchange(&g_stageEnemyCount, 0);
    InterlockedExchange(&g_stageRouteCount, 0);
}

static bool HasAnyLockActive()
{
    return g_lockInfAmmo || g_lockInfSp || g_lockGodHp || g_lockBattleDp || g_lockImmune ||
           g_ruleAllTerrain || g_ruleOverlap || g_ruleGlobalRange || g_ruleIgnoreMotion;
}

static void RefreshBattleSnapshot();

// 对战生命周期与脏状态守卫（运行在 Unity 主线程顶层消息泵，100% 原子无竞态）
static void PollBattleSessionGuard()
{
    Il2CppObject *bc = GetBattleControllerFast();
    int32_t st = 0;
    int32_t res = 0;
    bool loaded = false;
    bool autoRep = false;
    char lvlId[96] = { 0 };
    uint32_t frameCnt = 0;
    if (bc)
        ReadBattleControllerMeta(bc, &st, &autoRep, lvlId, sizeof(lvlId), &frameCnt, &res, &loaded);

    BattlePhase newPhase = PHASE_LOBBY;
    if (bc)
    {
        // 严格判定：只要 m_result != 0（通关/失败/撤退瞬间）或 m_state == 3，立刻视为结算中，
        // 第一时间停锁停写，彻底避开场景卸载与结算动画！
        if (st == 3 || res != 0)   newPhase = PHASE_FINISHED;
        else if (st == 2 && loaded) newPhase = PHASE_PLAYING;
        else                        newPhase = PHASE_LOADING;
    }

    const BattlePhase prevPhase = (BattlePhase)InterlockedExchange(&g_battlePhase, (LONG)newPhase);
    const bool prevAutoRep = (InterlockedExchange(&g_isAutoReplay, autoRep ? 1 : 0) != 0);
    InterlockedExchange(&g_inBattle, (newPhase == PHASE_PLAYING) ? 1 : 0);

    // 1) 战斗结束或返回大厅 -> 立即清空 Mod 本地锁与状态（绝不调用任何游戏结算期对象方法）
    if (prevPhase == PHASE_PLAYING && (newPhase == PHASE_FINISHED || newPhase == PHASE_LOBBY))
    {
        const bool hadLocks = HasAnyLockActive();
        const bool autoClr = (InterlockedCompareExchange(&g_autoResetNew, 0, 0) != 0);
        ResetPerBattleState(autoClr);
        if (newPhase == PHASE_FINISHED)
            SetFeedback(FB_INFO, hadLocks && autoClr
                        ? "大捷收工！已光速拔掉黑科技电源并重置状态（安全结算零闪退）"
                        : "战役结算中：物理法则改写器已锁仓，下局再浪！");
        else
            SetFeedback(FB_INFO, hadLocks && autoClr
                        ? "已撤回罗德岛大厅：全部黑科技已自动休眠归位"
                        : "已回到基地大厅：改写终端待命中，进关秒解锁");
        if (newPhase == PHASE_LOBBY)
        {
            g_lastBcPtr = nullptr;
            g_lastFrameCnt = 0;
            g_curLevelId[0] = 0;
        }
        return;
    }

    // 2) 进入全新战斗（换关 / 重开 / 肉鸽下一节点 / 撤退重进 / 逻辑帧归零）
    if (newPhase == PHASE_PLAYING)
    {
        const bool isNewSession =
            (bc != g_lastBcPtr) ||
            (prevPhase != PHASE_PLAYING) ||
            (lvlId[0] && g_curLevelId[0] && strcmp(lvlId, g_curLevelId) != 0) ||
            (g_lastFrameCnt > 30 && frameCnt < g_lastFrameCnt);

        if (isNewSession)
        {
            LONG sid = InterlockedIncrement(&g_battleSessId);
            const bool autoClr = (InterlockedCompareExchange(&g_autoResetNew, 0, 0) != 0);
            const bool mustClearLocks = autoClr || autoRep;
            ResetPerBattleState(mustClearLocks);

            g_lastBcPtr = bc;
            CopyStrSafe(g_curLevelId, sizeof(g_curLevelId), lvlId);
            g_lastFrameCnt = frameCnt;

            if (autoRep)
                SetFeedback(FB_WARN, "第 #%ld 局 [%s] 代理指挥录像中：已自动休眠所有持续锁，防止把完美自律轴改劈叉！",
                            (long)sid, lvlId[0] ? lvlId : "战斗关卡");
            else
                SetFeedback(FB_OK, "空投落地！已接管第 #%ld 局 [%s]（旧局状态已清零，物理法则任君拿捏）",
                            (long)sid, lvlId[0] ? lvlId : "战斗关卡");

            // 进入新局后立刻拉取一次编队与怪物库（无需等面板打开再等 1.5s）
            RefreshBattleSnapshot();
        }
        else
        {
            g_lastFrameCnt = frameCnt;
            if (!prevAutoRep && autoRep && HasAnyLockActive())
            {
                InterlockedExchange(&g_lockInfAmmo, 0);
                InterlockedExchange(&g_lockInfSp, 0);
                InterlockedExchange(&g_lockGodHp, 0);
                InterlockedExchange(&g_lockBattleDp, 0);
                InterlockedExchange(&g_lockImmune, 0);
                SetFeedback(FB_WARN, "检测到切入代理指挥：已自动暂停全部持续锁，保住你的自律录像！");
            }
        }
    }
    else if (newPhase == PHASE_LOBBY && g_lastBcPtr != nullptr)
    {
        g_lastBcPtr = nullptr;
        g_lastFrameCnt = 0;
        g_curLevelId[0] = 0;
        ResetPerBattleState(InterlockedCompareExchange(&g_autoResetNew, 0, 0) != 0);
    }
}

static int ScanStageEnemies()
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    if (!bc) return 0;

    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    FieldInfo *fLvl = ArkReflectFindField(bcKlass, "m_levelData", nullptr);
    if (!fLvl) return -2;
    Il2CppObject *lvlObj = api.il2cpp_field_get_value_object(fLvl, bc);
    if (!lvlObj) return 0;

    Il2CppClass *lk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(lvlObj) : lvlObj->klass;
    if (!lk) return 0;

    if (FieldInfo *fRoutes = ArkReflectFindField(lk, "routes", nullptr))
    {
        if (Il2CppObject *rArr = api.il2cpp_field_get_value_object(fRoutes, lvlObj))
            InterlockedExchange(&g_stageRouteCount, *(int32_t *)((char *)rArr + 0x18));
    }

    int count = 0;
    if (FieldInfo *fDbRefs = ArkReflectFindField(lk, "enemyDbRefs", nullptr))
    {
        if (Il2CppObject *arr = api.il2cpp_field_get_value_object(fDbRefs, lvlObj))
        {
            int32_t len = *(int32_t *)((char *)arr + 0x18);
            Il2CppObject **items = (Il2CppObject **)((char *)arr + 0x20);
            for (int32_t i = 0; i < len && count < 64; ++i)
            {
                Il2CppObject *refObj = items[i];
                if (!refObj) continue;
                Il2CppClass *rk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(refObj) : refObj->klass;
                if (!rk) continue;

                char idBuf[96] = { 0 };
                char nameBuf[96] = { 0 };
                if (FieldInfo *fId = ArkReflectFindField(rk, "id", nullptr))
                    Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fId, refObj), idBuf, sizeof(idBuf));
                if (FieldInfo *fOw = ArkReflectFindField(rk, "overwrittenData", nullptr))
                {
                    if (Il2CppObject *owObj = api.il2cpp_field_get_value_object(fOw, refObj))
                    {
                        Il2CppClass *ok = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(owObj) : owObj->klass;
                        if (ok)
                        {
                            if (FieldInfo *fNm = ArkReflectFindField(ok, "name", nullptr))
                            {
                                if (Il2CppObject *nmWrapper = api.il2cpp_field_get_value_object(fNm, owObj))
                                {
                                    Il2CppClass *wk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(nmWrapper) : nmWrapper->klass;
                                    if (wk)
                                    {
                                        if (FieldInfo *fVal = ArkReflectFindField(wk, "m_value", nullptr))
                                            Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fVal, nmWrapper), nameBuf, sizeof(nameBuf));
                                    }
                                }
                            }
                        }
                    }
                }
                if (!idBuf[0]) continue;
                CopyStrSafe(g_stageEnemies[count].id, sizeof(g_stageEnemies[count].id), idBuf);
                CopyStrSafe(g_stageEnemies[count].name, sizeof(g_stageEnemies[count].name), nameBuf);
                ++count;
            }
        }
    }

    InterlockedExchange(&g_stageEnemyCount, count);
    if (count > 0 && !g_spawnEnemyKey[0])
        CopyStrSafe(g_spawnEnemyKey, sizeof(g_spawnEnemyKey), g_stageEnemies[0].id);
    return count;
}

static void RefreshBattleSnapshot()
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    int32_t st = 0;
    if (bc) ReadBattleControllerMeta(bc, &st, nullptr, nullptr, 0, nullptr);
    const bool playing = (bc != nullptr && st == 2);

    if (!playing)
    {
        SpinLock(&g_snapLock);
        g_stageCharCount = 0;
        g_liveEnemyCount = 0;
        g_liveEnemyTotal = 0;
        SpinUnlock(&g_snapLock);
        InterlockedExchange(&g_stageEnemyCount, 0);
        return;
    }

    // 刚进入新关卡时自动拉取本关可用怪物列表（无需手动再点按钮）
    if (g_stageEnemyCount <= 0)
        ScanStageEnemies();

    StageCharInfo tmpChars[32];
    int cCount = 0;

    auto findOrAddChar = [&](const char *id, const char *nameCn) -> StageCharInfo * {
        if (!id || !id[0]) return nullptr;
        for (int i = 0; i < cCount; ++i)
        {
            if (strcmp(tmpChars[i].id, id) == 0)
            {
                if (!tmpChars[i].nameCn[0] && nameCn && nameCn[0])
                    CopyStrSafe(tmpChars[i].nameCn, sizeof(tmpChars[i].nameCn), nameCn);
                return &tmpChars[i];
            }
        }
        if (cCount >= 32) return nullptr;
        StageCharInfo *e = &tmpChars[cCount++];
        memset(e, 0, sizeof(*e));
        CopyStrSafe(e->id, sizeof(e->id), id);
        CopyStrSafe(e->nameCn, sizeof(e->nameCn), nameCn);
        return e;
    };

    // 1) 零 GC 停顿读取手牌编队 (BattleController.m_deckDict -> Deck.m_cards)
    {
        Il2CppObject *cards[128];
        int cn = GetBattleCardsFast(bc, cards, 128);
        for (int i = 0; i < cn; ++i)
        {
            Il2CppObject *card = cards[i];
            if (!card) continue;
            Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(card) : card->klass;
            if (!ck) continue;
            FieldInfo *fData  = ArkReflectFindField(ck, "m_data", nullptr);
            FieldInfo *fDelta = ArkReflectFindField(ck, "m_costDelta", nullptr);
            FieldInfo *fSnap  = ArkReflectFindField(ck, "<attributes>k__BackingField", nullptr);
            if (!fData) continue;

            char idBuf[96] = { 0 }, nmBuf[96] = { 0 };
            if (!ExtractCharIdName(api.il2cpp_field_get_value_object(fData, card),
                                   idBuf, sizeof(idBuf), nmBuf, sizeof(nmBuf)))
                continue;
            if (strncmp(idBuf, "char_", 5) != 0 && strncmp(idBuf, "token_", 6) != 0)
                continue;

            StageCharInfo *info = findOrAddChar(idBuf, nmBuf);
            if (!info) continue;
            info->inHand = true;

            int32_t delta = 0;
            if (fDelta)
            {
                if (void *a = ArkReflectFieldAddr(fDelta, card)) delta = *(int32_t *)a;
            }
            if (fSnap)
            {
                if (Il2CppObject *snap = api.il2cpp_field_get_value_object(fSnap, card))
                {
                    Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(snap) : snap->klass;
                    if (sk)
                    {
                        if (FieldInfo *fAttr = ArkReflectFindField(sk, "m_attributes", nullptr))
                        {
                            if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, snap))
                            {
                                int baseCost = (int)floorf(ReadAttributeOnObj(attrObj, 4) + 0.5f);
                                int effCost = baseCost + delta;
                                info->cost        = effCost < 0 ? 0 : effCost;
                                info->maxHp       = ReadAttributeOnObj(attrObj, 0);
                                info->hp          = info->maxHp;
                                info->atk         = ReadAttributeOnObj(attrObj, 1);
                                info->def         = ReadAttributeOnObj(attrObj, 2);
                                info->magicRes    = ReadAttributeOnObj(attrObj, 3);
                                info->blockCnt    = ReadAttributeOnObj(attrObj, 5);
                                info->atkSpd      = ReadAttributeOnObj(attrObj, 7);
                                info->baseAtkTime = ReadAttributeOnObj(attrObj, 8);
                            }
                        }
                    }
                }
            }
        }
    }

    // 2) 零 GC 停顿读取场上存活干员 (UnitManager.characters) + 零 GC 弹药计数器
    {
        Il2CppObject *chars[256];
        int n = GetBattleCharactersFast(bc, chars, 256);
        const ArkClassEntry *charCe = ArkReflectFindClass("Torappu.Battle.Character");
        Il2CppClass *baseCk = charCe ? charCe->klass : nullptr;

        for (int i = 0; i < n; ++i)
        {
            Il2CppObject *chr = chars[i];
            if (!chr) continue;
            Il2CppClass *ck = baseCk ? baseCk : (api.il2cpp_object_get_class ? api.il2cpp_object_get_class(chr) : chr->klass);
            if (!ck || !IsEntityAlive(chr, ck)) continue;

            FieldInfo *fData  = ArkReflectFindField(ck, "<data>k__BackingField", nullptr);
            FieldInfo *fAttr  = ArkReflectFindField(ck, "m_attributes", nullptr);
            FieldInfo *fHp    = ArkReflectFindField(ck, "m_hp", nullptr);
            FieldInfo *fSp    = ArkReflectFindField(ck, "m_sp", nullptr);
            FieldInfo *fMaxSp = ArkReflectFindField(ck, "<maxSp>k__BackingField", nullptr);
            if (!fData) continue;

            char idBuf[96] = { 0 }, nmBuf[96] = { 0 };
            if (!ExtractCharIdName(api.il2cpp_field_get_value_object(fData, chr),
                                   idBuf, sizeof(idBuf), nmBuf, sizeof(nmBuf)))
                continue;
            if (strncmp(idBuf, "char_", 5) != 0 && strncmp(idBuf, "token_", 6) != 0)
                continue;

            StageCharInfo *info = findOrAddChar(idBuf, nmBuf);
            if (!info) continue;
            info->deployed = true;

            if (fAttr)
            {
                if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, chr))
                {
                    info->maxHp       = ReadAttributeOnObj(attrObj, 0);
                    info->atk         = ReadAttributeOnObj(attrObj, 1);
                    info->def         = ReadAttributeOnObj(attrObj, 2);
                    info->magicRes    = ReadAttributeOnObj(attrObj, 3);
                    info->blockCnt    = ReadAttributeOnObj(attrObj, 5);
                    info->atkSpd      = ReadAttributeOnObj(attrObj, 7);
                    info->baseAtkTime = ReadAttributeOnObj(attrObj, 8);
                }
            }
            if (fHp)
            {
                if (void *a = ArkReflectFieldAddr(fHp, chr))
                    info->hp = (float)((double)(*(int64_t *)a) / 4294967296.0);
            }
            if (fSp)
            {
                if (void *a = ArkReflectFieldAddr(fSp, chr))
                    info->sp = ReadObscuredFpSlot(a);
            }
            if (fMaxSp)
            {
                if (void *a = ArkReflectFieldAddr(fMaxSp, chr))
                    info->maxSp = (float)(*(int32_t *)a);
            }

            Il2CppObject *myCnts[4];
            if (FindCharAmmoCounters(chr, ck, myCnts, 4, nullptr, 0) > 0)
            {
                int32_t r = 0, m = 0;
                if (IsCounterValid(myCnts[0], &r, &m))
                {
                    info->hasAmmo = true;
                    info->ammoRem = r;
                    info->ammoMax = m;
                }
            }
        }
    }

    // 3) 零 GC 停顿读取场上存活怪物 (UnitManager.enemies)
    LiveEnemyGroup tmpEnemies[32];
    int eGrpCount = 0;
    int eTotal = 0;
    {
        Il2CppObject *enms[256];
        int en = GetBattleEnemiesFast(bc, enms, 256);
        const ArkClassEntry *eCe = ArkReflectFindClass("Torappu.Battle.Enemy");
        Il2CppClass *baseEk = eCe ? eCe->klass : nullptr;

        for (int i = 0; i < en; ++i)
        {
            Il2CppObject *enm = enms[i];
            if (!enm) continue;
            Il2CppClass *ek = baseEk ? baseEk : (api.il2cpp_object_get_class ? api.il2cpp_object_get_class(enm) : enm->klass);
            if (!ek || !IsEntityAlive(enm, ek)) continue;

            FieldInfo *fAttr = ArkReflectFindField(ek, "m_attributes", nullptr);
            FieldInfo *fHp   = ArkReflectFindField(ek, "m_hp", nullptr);

            char keyBuf[96] = { 0 }, nmBuf[96] = { 0 };
            if (!ExtractEnemyKeyName(enm, ek, keyBuf, sizeof(keyBuf), nmBuf, sizeof(nmBuf)))
                continue;
            ++eTotal;

            LiveEnemyGroup *grp = nullptr;
            for (int g = 0; g < eGrpCount; ++g)
            {
                if (strcmp(tmpEnemies[g].key, keyBuf) == 0) { grp = &tmpEnemies[g]; break; }
            }
            if (!grp && eGrpCount < 32)
            {
                grp = &tmpEnemies[eGrpCount++];
                memset(grp, 0, sizeof(*grp));
                CopyStrSafe(grp->key, sizeof(grp->key), keyBuf);
                CopyStrSafe(grp->name, sizeof(grp->name), nmBuf);
                if (fAttr)
                {
                    if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, enm))
                    {
                        grp->maxHp   = ReadAttributeOnObj(attrObj, 0);
                        grp->atk     = ReadAttributeOnObj(attrObj, 1);
                        grp->def     = ReadAttributeOnObj(attrObj, 2);
                        grp->moveSpd = ReadAttributeOnObj(attrObj, 6);
                    }
                }
                if (fHp)
                {
                    if (void *a = ArkReflectFieldAddr(fHp, enm))
                        grp->hp = (float)((double)(*(int64_t *)a) / 4294967296.0);
                }
            }
            if (grp) grp->count++;
        }
    }

    SpinLock(&g_snapLock);
    g_stageCharCount = cCount;
    for (int i = 0; i < cCount; ++i) g_stageChars[i] = tmpChars[i];
    g_liveEnemyCount = eGrpCount;
    g_liveEnemyTotal = eTotal;
    for (int i = 0; i < eGrpCount; ++i) g_liveEnemies[i] = tmpEnemies[i];
    SpinUnlock(&g_snapLock);
}

// ============================================================================
// 各项修改执行函数（100% 运行在 Unity 主线程顶层消息泵 + 幂等防重复修改）
// ============================================================================
static int ApplyCardCostDelta(const char *filter, int32_t costDelta, char *lastMatched, int cap, int *outSkipped = nullptr)
{
    if (outSkipped) *outSkipped = 0;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *ce = ArkReflectFindClass("CharacterCard");

    Il2CppObject *objs[128];
    int n = bc ? GetBattleCardsFast(bc, objs, 128) : -1;
    if (n < 0 && ce && ce->klass) n = ArkGcEnumObjects(ce->klass, objs, 128, 1);
    if (n <= 0) return 0;

    int hit = 0;
    int skipped = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *card = objs[i];
        if (!card) continue;
        Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(card) : card->klass;
        if (!ck) continue;
        FieldInfo *fData  = ArkReflectFindField(ck, "m_data", nullptr);
        FieldInfo *fDelta = ArkReflectFindField(ck, "m_costDelta", nullptr);
        FieldInfo *fSnap  = ArkReflectFindField(ck, "<attributes>k__BackingField", nullptr);
        FieldInfo *fDeck  = ArkReflectFindField(ck, "m_deck", nullptr);
        if (!fData || !fDelta) continue;
        Il2CppObject *dataObj = api.il2cpp_field_get_value_object(fData, card);
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(dataObj, filter, desc, sizeof(desc)))
            continue;

        // 幂等检查：若目标设为 0 费且当前有效费用已 <= 0，跳过重复修改与 UI 刷新
        int32_t curDelta = 0;
        if (void *a = ArkReflectFieldAddr(fDelta, card)) curDelta = *(int32_t *)a;
        Il2CppObject *snap = fSnap ? api.il2cpp_field_get_value_object(fSnap, card) : nullptr;
        Il2CppClass *sk = snap ? (api.il2cpp_object_get_class ? api.il2cpp_object_get_class(snap) : snap->klass) : nullptr;
        Il2CppObject *innerAttr = nullptr;
        if (sk)
        {
            if (FieldInfo *fInnerAttr = ArkReflectFindField(sk, "m_attributes", nullptr))
                innerAttr = api.il2cpp_field_get_value_object(fInnerAttr, snap);
        }
        if (costDelta <= -50)
        {
            int curBase = innerAttr ? (int)floorf(ReadAttributeOnObj(innerAttr, 4) + 0.5f) : 0;
            if (curBase <= 0 && curDelta <= -50)
            {
                CopyStrSafe(lastMatched, cap, desc);
                ++skipped;
                continue;
            }
        }

        ArkReflectWriteFieldNum(fDelta, card, (double)costDelta);

        if (snap && sk)
        {
            if (innerAttr && costDelta <= -50)
                OverwriteAttributeOnObj(innerAttr, 4 /* COST */, 0.0f);
            if (MethodInfo *mModCost = api.il2cpp_class_get_method_from_name(sk, "ModifyCost", 1))
            {
                int32_t targetCost = (costDelta <= -50) ? 0 : costDelta;
                void *cargs[1] = { &targetCost };
                Il2CppException *exc = nullptr;
                api.il2cpp_runtime_invoke(mModCost, snap, cargs, &exc);
            }
        }

        if (fDeck)
        {
            if (Il2CppObject *deckObj = api.il2cpp_field_get_value_object(fDeck, card))
            {
                Il2CppClass *dk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(deckObj) : deckObj->klass;
                if (dk)
                {
                    if (MethodInfo *mOnCost = api.il2cpp_class_get_method_from_name(dk, "OnCardCostChanged", 1))
                    {
                        void *dargs[1] = { card };
                        Il2CppException *exc = nullptr;
                        api.il2cpp_runtime_invoke(mOnCost, deckObj, dargs, &exc);
                    }
                }
            }
        }

        CopyStrSafe(lastMatched, cap, desc);
        ++hit;
    }
    if (outSkipped) *outSkipped = skipped;
    return hit;
}

static int ApplyCardResetRespawn(const char *filter, char *lastMatched, int cap, int *outSkipped = nullptr)
{
    if (outSkipped) *outSkipped = 0;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *ce = ArkReflectFindClass("CharacterCard");

    Il2CppObject *objs[128];
    int n = bc ? GetBattleCardsFast(bc, objs, 128) : -1;
    if (n < 0 && ce && ce->klass) n = ArkGcEnumObjects(ce->klass, objs, 128, 1);
    if (n <= 0) return 0;

    int hit = 0;
    int skipped = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *card = objs[i];
        if (!card) continue;
        Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(card) : card->klass;
        if (!ck) continue;
        FieldInfo *fData = ArkReflectFindField(ck, "m_data", nullptr);
        FieldInfo *fSnap = ArkReflectFindField(ck, "<attributes>k__BackingField", nullptr);
        if (!fData) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, card), filter, desc, sizeof(desc)))
            continue;

        MethodInfo *mReset = nullptr;
        Il2CppClass *cur = ck;
        while (cur && !mReset)
        {
            mReset = api.il2cpp_class_get_method_from_name(cur, "ResetRespawnTime", 1);
            cur = api.il2cpp_class_get_parent(cur);
        }

        bool needAttrWrite = true;
        if (fSnap)
        {
            if (Il2CppObject *snap = api.il2cpp_field_get_value_object(fSnap, card))
            {
                Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(snap) : snap->klass;
                if (sk)
                {
                    if (FieldInfo *fInnerAttr = ArkReflectFindField(sk, "m_attributes", nullptr))
                    {
                        if (Il2CppObject *innerAttr = api.il2cpp_field_get_value_object(fInnerAttr, snap))
                        {
                            float curResp = ReadAttributeOnObj(innerAttr, 21 /* RESPAWN_TIME */);
                            if (curResp <= 1.01f && curResp > 0.0f)
                                needAttrWrite = false;
                            else
                                OverwriteAttributeOnObj(innerAttr, 21 /* RESPAWN_TIME */, 1.0f);
                        }
                    }
                }
            }
        }
        if (mReset)
        {
            uint8_t waitFirst = 0;
            void *args[1] = { &waitFirst };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mReset, card, args, &exc);
        }
        CopyStrSafe(lastMatched, cap, desc);
        if (needAttrWrite) ++hit;
        else ++skipped;
    }
    if (outSkipped) *outSkipped = skipped;
    return hit;
}

static int ApplyFreeDeployLimit(const char *filter, bool *outLimitAlreadyBoosted = nullptr)
{
    if (outLimitAlreadyBoosted) *outLimitAlreadyBoosted = false;
    Il2CppApi &api = ir::Api();
    int hit = 0;
    Il2CppObject *bc = GetBattleControllerFast();

    {
        Il2CppObject *objs[128];
        int n = bc ? GetBattleCardsFast(bc, objs, 128) : -1;
        if (n < 0)
        {
            if (const ArkClassEntry *ce = ArkReflectFindClass("CharacterCard"))
                if (ce->klass) n = ArkGcEnumObjects(ce->klass, objs, 128, 1);
        }
        for (int i = 0; i < n; ++i)
        {
            Il2CppObject *card = objs[i];
            if (!card) continue;
            Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(card) : card->klass;
            if (!ck) continue;
            FieldInfo *fData  = ArkReflectFindField(ck, "m_data", nullptr);
            FieldInfo *fDont1 = ArkReflectFindField(ck, "m_dontOccupyDeployCnt", nullptr);
            FieldInfo *fDont2 = ArkReflectFindField(ck, "m_dontOccupyMaxDeployCnt", nullptr);
            if (!fData) continue;
            char desc[128] = { 0 };
            if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, card), filter, desc, sizeof(desc)))
                continue;
            bool changed = false;
            if (fDont1)
            {
                if (void *a = ArkReflectFieldAddr(fDont1, card))
                    if (*(uint8_t *)a == 0) { ArkReflectWriteFieldNum(fDont1, card, 1.0); changed = true; }
            }
            if (fDont2)
            {
                if (void *a = ArkReflectFieldAddr(fDont2, card))
                    if (*(uint8_t *)a == 0) { ArkReflectWriteFieldNum(fDont2, card, 1.0); changed = true; }
            }
            if (changed) ++hit;
        }
    }

    // 幂等防叠加：单局只允许对 BattleController 执行一次 +99 部署上限，绝不重复叠成 +198/+297
    if (g_applied.deployLimit)
    {
        if (outLimitAlreadyBoosted) *outLimitAlreadyBoosted = true;
    }
    else if (bc)
    {
        Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
        if (bcKlass)
        {
            if (MethodInfo *mModLim = api.il2cpp_class_get_method_from_name(bcKlass, "ModifyCharacterLimit", 3))
            {
                int32_t addVal = 99;
                Il2CppObject *src = nullptr;
                int32_t side = 0;
                void *args[3] = { &addVal, src, &side };
                Il2CppException *exc = nullptr;
                api.il2cpp_runtime_invoke(mModLim, bc, args, &exc);
                if (!exc) g_applied.deployLimit = true;
            }
        }
    }
    return hit;
}

static int ApplyCharacterAttribute(const char *filter, int32_t attrType, float val,
                                   char *lastMatched, int cap, float *outBeforeVal = nullptr, int *outSkipped = nullptr)
{
    if (outSkipped) *outSkipped = 0;
    Il2CppApi &api = ir::Api();
    int hit = 0;
    int skipped = 0;
    Il2CppObject *bc = GetBattleControllerFast();

    if (const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character"))
    {
        if (ce->klass)
        {
            Il2CppObject *objs[256];
            int n = bc ? GetBattleCharactersFast(bc, objs, 256) : -1;
            if (n < 0) n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
            FieldInfo *fData = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
            FieldInfo *fId   = ArkReflectFindField(ce->klass, "<id>k__BackingField", nullptr);
            FieldInfo *fAttr = ArkReflectFindField(ce->klass, "m_attributes", nullptr);

            for (int i = 0; i < n; ++i)
            {
                Il2CppObject *chr = objs[i];
                if (!chr || !fAttr || !IsEntityAlive(chr, ce->klass)) continue;

                char desc[128] = { 0 };
                bool matched = false;
                if (fData)
                {
                    if (Il2CppObject *dataObj = api.il2cpp_field_get_value_object(fData, chr))
                        matched = CheckCharDataMatches(dataObj, filter, desc, sizeof(desc));
                }
                if (!matched && fId)
                {
                    char idStr[96] = { 0 };
                    Il2CppStrToUtf8(api.il2cpp_field_get_value_object(fId, chr), idStr, sizeof(idStr));
                    if (idStr[0] && strncmp(idStr, "char_", 5) == 0 && MatchSubstringIgnoreCase(idStr, filter))
                    {
                        CopyStrSafe(desc, sizeof(desc), idStr);
                        matched = true;
                    }
                }
                if (!matched) continue;

                Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, chr);
                float curVal = attrObj ? ReadAttributeOnObj(attrObj, attrType) : 0.0f;
                if (outBeforeVal && hit == 0 && skipped == 0 && attrObj)
                    *outBeforeVal = curVal;

                if (attrObj && fabsf(curVal - val) < 0.01f)
                {
                    CopyStrSafe(lastMatched, cap, desc);
                    ++skipped;
                    continue;
                }

                if (OverwriteAttributeOnObj(attrObj, attrType, val))
                {
                    if (attrType == 0 /* MAX_HP */)
                        SetEntityCurrentHp(chr, ce->klass, val);
                    else if (attrType == 14 /* SP_RECOVERY_PER_SEC */)
                        FillEntitySp(chr, ce->klass);

                    CopyStrSafe(lastMatched, cap, desc);
                    ++hit;
                }
            }
        }
    }

    // 同步写入手牌快照（幂等跳过已是目标值的手牌）
    {
        Il2CppObject *cards[128];
        int cn = bc ? GetBattleCardsFast(bc, cards, 128) : -1;
        if (cn < 0)
        {
            if (const ArkClassEntry *cardCe = ArkReflectFindClass("CharacterCard"))
                if (cardCe->klass) cn = ArkGcEnumObjects(cardCe->klass, cards, 128, 1);
        }
        for (int i = 0; i < cn; ++i)
        {
            Il2CppObject *card = cards[i];
            if (!card) continue;
            Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(card) : card->klass;
            if (!ck) continue;
            FieldInfo *fData = ArkReflectFindField(ck, "m_data", nullptr);
            FieldInfo *fSnap = ArkReflectFindField(ck, "<attributes>k__BackingField", nullptr);
            if (!fData || !fSnap) continue;
            char desc[128] = { 0 };
            if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, card), filter, desc, sizeof(desc)))
                continue;
            Il2CppObject *snap = api.il2cpp_field_get_value_object(fSnap, card);
            if (!snap) continue;
            Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(snap) : snap->klass;
            if (!sk) continue;
            if (FieldInfo *fInnerAttr = ArkReflectFindField(sk, "m_attributes", nullptr))
            {
                if (Il2CppObject *innerAttr = api.il2cpp_field_get_value_object(fInnerAttr, snap))
                {
                    if (fabsf(ReadAttributeOnObj(innerAttr, attrType) - val) >= 0.01f)
                    {
                        if (OverwriteAttributeOnObj(innerAttr, attrType, val) && hit == 0)
                            CopyStrSafe(lastMatched, cap, desc);
                    }
                }
            }
        }
    }
    if (outSkipped) *outSkipped = skipped;
    return hit;
}

static int ApplyCharacterFullHpSp(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!ce || !ce->klass) return 0;

    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *objs[256];
    int n = bc ? GetBattleCharactersFast(bc, objs, 256) : -1;
    if (n < 0) n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
    FieldInfo *fData = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    FieldInfo *fAttr = ArkReflectFindField(ce->klass, "m_attributes", nullptr);

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *chr = objs[i];
        if (!chr || !fData || !IsEntityAlive(chr, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
            continue;

        float maxHp = 10000.0f;
        if (fAttr)
        {
            if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, chr))
            {
                float r = ReadAttributeOnObj(attrObj, 0);
                if (r > 0.0f) maxHp = r;
            }
        }
        SetEntityCurrentHp(chr, ce->klass, maxHp);
        FillEntitySp(chr, ce->klass);
        CopyStrSafe(lastMatched, cap, desc);
        ++hit;
    }
    return hit;
}

static bool ApplyBattleDp(int32_t targetDp, bool *outAlreadyAtTarget = nullptr)
{
    if (outAlreadyAtTarget) *outAlreadyAtTarget = false;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    if (!bc) return false;
    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bcKlass) return false;

    int32_t side = 0;
    int32_t wantMax = (targetDp > 999) ? targetDp : 999;

    // 幂等检查当前上限与当前 DP，避免每帧/每次重复调 SetMaxCost / SetCost
    int32_t curMax = -1;
    if (MethodInfo *mGetMax = api.il2cpp_class_get_method_from_name(bcKlass, "GetMaxCost", 1))
    {
        void *args[1] = { &side };
        Il2CppException *exc = nullptr;
        Il2CppObject *ret = api.il2cpp_runtime_invoke(mGetMax, bc, args, &exc);
        if (!exc && ret && api.il2cpp_object_unbox)
            curMax = *(int32_t *)api.il2cpp_object_unbox(ret);
    }
    if (curMax < wantMax)
    {
        if (MethodInfo *mSetMax = api.il2cpp_class_get_method_from_name(bcKlass, "SetMaxCost", 2))
        {
            void *args[2] = { &wantMax, &side };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetMax, bc, args, &exc);
        }
    }

    int32_t curCost = -1;
    if (MethodInfo *mGetCost = api.il2cpp_class_get_method_from_name(bcKlass, "GetCost", 1))
    {
        void *args[1] = { &side };
        Il2CppException *exc = nullptr;
        Il2CppObject *ret = api.il2cpp_runtime_invoke(mGetCost, bc, args, &exc);
        if (!exc && ret && api.il2cpp_object_unbox)
            curCost = *(int32_t *)api.il2cpp_object_unbox(ret);
    }
    if (curCost >= targetDp)
    {
        if (outAlreadyAtTarget) *outAlreadyAtTarget = true;
        return true;
    }

    if (MethodInfo *mSetCost = api.il2cpp_class_get_method_from_name(bcKlass, "SetCost", 2))
    {
        void *args[2] = { &targetDp, &side };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mSetCost, bc, args, &exc);
        return exc == nullptr;
    }
    return false;
}

static bool ApplyBattleLifePoint(int32_t targetLife, bool *outAlreadyAtTarget = nullptr)
{
    if (outAlreadyAtTarget) *outAlreadyAtTarget = false;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    if (!bc) return false;
    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bcKlass) return false;

    int32_t side = 0;
    if (MethodInfo *mGetLp = api.il2cpp_class_get_method_from_name(bcKlass, "GetLifePoint", 1))
    {
        void *args[1] = { &side };
        Il2CppException *exc = nullptr;
        Il2CppObject *ret = api.il2cpp_runtime_invoke(mGetLp, bc, args, &exc);
        if (!exc && ret && api.il2cpp_object_unbox)
        {
            int32_t curLp = *(int32_t *)api.il2cpp_object_unbox(ret);
            if (curLp >= targetLife)
            {
                if (outAlreadyAtTarget) *outAlreadyAtTarget = true;
                g_applied.life999 = true;
                return true;
            }
        }
    }

    if (MethodInfo *mSetLp = api.il2cpp_class_get_method_from_name(bcKlass, "SetLifePoint", 3))
    {
        uint8_t force = 1;
        void *args[3] = { &targetLife, &side, &force };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mSetLp, bc, args, &exc);
        if (exc == nullptr) g_applied.life999 = true;
        return exc == nullptr;
    }
    return false;
}

static int ApplyEnemyAttribute(const char *filter, int32_t attrType, float val,
                               char *lastMatched, int cap, float *outBeforeVal = nullptr, int *outSkipped = nullptr)
{
    if (outSkipped) *outSkipped = 0;
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass) return -1;

    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *objs[256];
    int n = bc ? GetBattleEnemiesFast(bc, objs, 256) : -1;
    if (n < 0) n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
    FieldInfo *fAttr = ArkReflectFindField(ce->klass, "m_attributes", nullptr);
    if (!fAttr) return -2;

    int hit = 0;
    int skipped = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *enm = objs[i];
        if (!enm || !IsEntityAlive(enm, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckEnemyMatches(enm, ce->klass, filter, desc, sizeof(desc)))
            continue;

        Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, enm);
        float curVal = attrObj ? ReadAttributeOnObj(attrObj, attrType) : 0.0f;
        if (outBeforeVal && hit == 0 && skipped == 0 && attrObj)
            *outBeforeVal = curVal;

        if (attrObj && fabsf(curVal - val) < 0.01f)
        {
            CopyStrSafe(lastMatched, cap, desc);
            ++skipped;
            continue;
        }

        if (OverwriteAttributeOnObj(attrObj, attrType, val))
        {
            if (attrType == 0 /* MAX_HP */)
                SetEntityCurrentHp(enm, ce->klass, val);
            CopyStrSafe(lastMatched, cap, desc);
            ++hit;
        }
    }
    if (outSkipped) *outSkipped = skipped;
    return hit;
}

static int ApplyEnemyUnharmful(const char *filter, char *lastMatched, int cap, int *outSkipped = nullptr)
{
    if (outSkipped) *outSkipped = 0;
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass) return 0;

    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *objs[256];
    int n = bc ? GetBattleEnemiesFast(bc, objs, 256) : -1;
    if (n < 0) n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
    FieldInfo *fLpRed = ArkReflectFindField(ce->klass, "<lifePointReduce>k__BackingField", nullptr);
    MethodInfo *mUnharm = api.il2cpp_class_get_method_from_name(ce->klass, "MarkUnharmful", 0);

    int hit = 0;
    int skipped = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *enm = objs[i];
        if (!enm || !IsEntityAlive(enm, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckEnemyMatches(enm, ce->klass, filter, desc, sizeof(desc)))
            continue;

        if (fLpRed)
        {
            if (void *a = ArkReflectFieldAddr(fLpRed, enm))
            {
                if (*(int32_t *)a == 0)
                {
                    CopyStrSafe(lastMatched, cap, desc);
                    ++skipped;
                    continue;
                }
            }
            ArkReflectWriteFieldNum(fLpRed, enm, 0.0);
        }
        if (mUnharm)
        {
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mUnharm, enm, nullptr, &exc);
        }
        CopyStrSafe(lastMatched, cap, desc);
        ++hit;
    }
    if (outSkipped) *outSkipped = skipped;
    return hit;
}

static int ApplyEnemyKill(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass) return 0;

    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *objs[256];
    int n = bc ? GetBattleEnemiesFast(bc, objs, 256) : -1;
    if (n < 0) n = ArkGcEnumObjects(ce->klass, objs, 256, 1);

    MethodInfo *mKill = nullptr;
    Il2CppClass *cur = ce->klass;
    while (cur && !mKill)
    {
        mKill = api.il2cpp_class_get_method_from_name(cur, "FinishWithNoReason", 0);
        cur = api.il2cpp_class_get_parent(cur);
    }

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *enm = objs[i];
        if (!enm || !IsEntityAlive(enm, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckEnemyMatches(enm, ce->klass, filter, desc, sizeof(desc)))
            continue;

        if (mKill)
        {
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mKill, enm, nullptr, &exc);
        }
        else
        {
            SetEntityCurrentHp(enm, ce->klass, 0.0f);
        }
        CopyStrSafe(lastMatched, cap, desc);
        ++hit;
    }
    return hit;
}

// ============================================================================
// 底层战斗规则粉碎机 & 进阶整活引擎（100% Unity 主线程 / 零 GC 停顿 / 可随时还原）
// ============================================================================
static void *StructMemberAddr(void *structBase, Il2CppClass *structKlass, const char *memberName)
{
    if (!structBase || !structKlass || !memberName) return nullptr;
    Il2CppApi &api = ir::Api();
    if (!api.il2cpp_field_get_offset) return nullptr;
    FieldInfo *f = ArkReflectFindField(structKlass, memberName, nullptr);
    if (!f) return nullptr;
    size_t rawOff = api.il2cpp_field_get_offset(f);
    if (api.il2cpp_class_is_valuetype && api.il2cpp_class_is_valuetype(structKlass))
    {
        if (rawOff >= sizeof(Il2CppObject))
            rawOff -= sizeof(Il2CppObject);
    }
    return (char *)structBase + rawOff;
}

static MethodInfo *FindMethodWalk(Il2CppClass *klass, const char *name, int argsCount)
{
    Il2CppApi &api = ir::Api();
    if (!klass || !name || !api.il2cpp_class_get_method_from_name) return nullptr;
    Il2CppClass *cur = klass;
    while (cur)
    {
        if (MethodInfo *m = api.il2cpp_class_get_method_from_name(cur, name, argsCount))
            return m;
        cur = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(cur) : nullptr;
    }
    return nullptr;
}

// 精确查找指定参数类型后缀的非泛型方法（避免误拿 GetComponent<T> / AddComponent<T> 泛型定义崩溃）
static MethodInfo *FindMethodWithParamType(Il2CppClass *klass, const char *name, int paramCount, const char *paramSuffix)
{
    Il2CppApi &api = ir::Api();
    if (!klass || !name || !api.il2cpp_class_get_methods) return nullptr;
    Il2CppClass *cur = klass;
    while (cur)
    {
        void *iter = nullptr;
        while (MethodInfo *m = api.il2cpp_class_get_methods(cur, &iter))
        {
            const char *mn = api.il2cpp_method_get_name ? api.il2cpp_method_get_name(m) : nullptr;
            if (!mn || strcmp(mn, name) != 0) continue;
            if (api.il2cpp_method_is_generic && api.il2cpp_method_is_generic(m)) continue;
            MethodInfoHeader *hdr = (MethodInfoHeader *)m;
            if (!hdr->methodPointer) continue;
            if (api.il2cpp_method_get_param_count && (int)api.il2cpp_method_get_param_count(m) != paramCount)
                continue;
            if (paramCount > 0 && paramSuffix && paramSuffix[0] && api.il2cpp_method_get_param && api.il2cpp_type_get_name)
            {
                Il2CppType *pt = api.il2cpp_method_get_param(m, 0);
                const char *ptn = pt ? api.il2cpp_type_get_name(pt) : nullptr;
                if (!ptn) continue;
                size_t len = strlen(ptn), slen = strlen(paramSuffix);
                if (len < slen || strcmp(ptn + (len - slen), paramSuffix) != 0)
                    continue;
            }
            return m;
        }
        cur = api.il2cpp_class_get_parent ? api.il2cpp_class_get_parent(cur) : nullptr;
    }
    return nullptr;
}

static bool IsUnityObjAlive(Il2CppObject *obj)
{
    if (!obj) return false;
    Il2CppApi &api = ir::Api();
    Il2CppClass *k = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(obj) : obj->klass;
    if (!k) return false;
    if (FieldInfo *fPtr = ArkReflectFindField(k, "m_CachedPtr", nullptr))
    {
        if (void *a = ArkReflectFieldAddr(fPtr, obj))
            return (*(void **)a != nullptr);
    }
    return true;
}

static Il2CppObject *GetBattleMapFast(Il2CppObject *bc)
{
    Il2CppApi &api = ir::Api();
    if (const ArkClassEntry *mapCe = ArkReflectFindClass("Torappu.Battle.Map"))
    {
        if (mapCe->klass && api.il2cpp_field_static_get_value)
        {
            if (FieldInfo *fInst = ArkReflectFindField(mapCe->klass, "s_instance", nullptr))
            {
                Il2CppObject *inst = nullptr;
                api.il2cpp_field_static_get_value(fInst, &inst);
                if (inst && IsUnityObjAlive(inst)) return inst;
            }
        }
    }
    if (bc)
    {
        Il2CppClass *bk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
        if (bk)
        {
            if (FieldInfo *fMap = ArkReflectFindField(bk, "_map", nullptr))
            {
                Il2CppObject *m = api.il2cpp_field_get_value_object(fMap, bc);
                if (m && IsUnityObjAlive(m)) return m;
            }
            if (MethodInfo *mGetMap = FindMethodWalk(bk, "get_map", 0))
            {
                Il2CppException *exc = nullptr;
                Il2CppObject *m = api.il2cpp_runtime_invoke(mGetMap, bc, nullptr, &exc);
                if (!exc && m && IsUnityObjAlive(m)) return m;
            }
        }
    }
    return nullptr;
}

static int GetMapTilesFast(Il2CppObject *mapObj, Il2CppObject **outTiles, int maxOut)
{
    if (!mapObj || !outTiles || maxOut <= 0) return 0;
    Il2CppApi &api = ir::Api();
    Il2CppClass *mk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(mapObj) : mapObj->klass;
    if (!mk) return 0;

    Il2CppObject *tileArr = nullptr;
    if (MethodInfo *mGetTiles = FindMethodWalk(mk, "get_tiles", 0))
    {
        Il2CppException *exc = nullptr;
        Il2CppObject *ret = api.il2cpp_runtime_invoke(mGetTiles, mapObj, nullptr, &exc);
        if (!exc && ret) tileArr = ret;
    }
    if (!tileArr)
    {
        if (FieldInfo *fT2d = ArkReflectFindField(mk, "_tiles", nullptr))
        {
            if (Il2CppObject *t2d = api.il2cpp_field_get_value_object(fT2d, mapObj))
            {
                Il2CppClass *t2k = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(t2d) : t2d->klass;
                if (t2k)
                {
                    if (FieldInfo *fArr = ArkReflectFindField(t2k, "_tiles", nullptr))
                        tileArr = api.il2cpp_field_get_value_object(fArr, t2d);
                }
            }
        }
    }
    if (!tileArr) return 0;

    int32_t len = *(int32_t *)((char *)tileArr + 0x18);
    if (len <= 0 || len > 4096) return 0;
    Il2CppObject **items = (Il2CppObject **)((char *)tileArr + 0x20);
    int n = 0;
    for (int32_t i = 0; i < len && n < maxOut; ++i)
    {
        if (items[i] && IsUnityObjAlive(items[i]))
            outTiles[n++] = items[i];
    }
    return n;
}

static bool InvokeBoolGetter(Il2CppObject *obj, Il2CppClass *k, const char *getterName)
{
    Il2CppApi &api = ir::Api();
    if (!obj || !k || !api.il2cpp_runtime_invoke || !api.il2cpp_object_unbox) return false;
    MethodInfo *m = FindMethodWalk(k, getterName, 0);
    if (!m) return false;
    Il2CppException *exc = nullptr;
    Il2CppObject *ret = api.il2cpp_runtime_invoke(m, obj, nullptr, &exc);
    if (exc || !ret) return false;
    uint8_t *b = (uint8_t *)api.il2cpp_object_unbox(ret);
    return b && (*b != 0);
}

static bool IsProtectedSpecialTile(Il2CppObject *tile, Il2CppClass *tk)
{
    if (!tile || !tk) return true;
    // 铁律：严禁触碰红门(刷怪口) / 蓝门(防守点) / 传送门，确保波次与通关判定 100% 稳定
    if (InvokeBoolGetter(tile, tk, "get_isStartPosTile")) return true;
    if (InvokeBoolGetter(tile, tk, "get_isEndPosTile"))   return true;
    if (InvokeBoolGetter(tile, tk, "get_isTelTile"))      return true;
    return false;
}

static void MarkMapBuildableDirty(Il2CppObject *mapObj)
{
    if (!mapObj) return;
    Il2CppApi &api = ir::Api();
    Il2CppClass *mk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(mapObj) : mapObj->klass;
    if (!mk) return;
    if (MethodInfo *mDirty = FindMethodWalk(mk, "SetBuildableDirty", 1))
    {
        uint8_t dirty = 1;
        void *args[1] = { &dirty };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mDirty, mapObj, args, &exc);
    }
}

static int32_t InvokeIntGetter(Il2CppObject *obj, Il2CppClass *k, const char *getterName, int32_t fallback)
{
    Il2CppApi &api = ir::Api();
    if (!obj || !k || !api.il2cpp_runtime_invoke || !api.il2cpp_object_unbox) return fallback;
    MethodInfo *m = FindMethodWalk(k, getterName, 0);
    if (!m) return fallback;
    Il2CppException *exc = nullptr;
    Il2CppObject *ret = api.il2cpp_runtime_invoke(m, obj, nullptr, &exc);
    if (exc || !ret) return fallback;
    int32_t *v = (int32_t *)api.il2cpp_object_unbox(ret);
    return v ? *v : fallback;
}

static TileBackup *FindOrRecordTileBackup(Il2CppObject *tile, Il2CppClass *tk)
{
    for (int i = 0; i < g_tileBackupCount; ++i)
        if (g_tileBackups[i].tile == tile) return &g_tileBackups[i];
    if (g_tileBackupCount >= 512) return nullptr;

    Il2CppApi &api = ir::Api();
    FieldInfo *fData = ArkReflectFindField(tk, "m_data", nullptr);
    Il2CppObject *td = fData ? api.il2cpp_field_get_value_object(fData, tile) : nullptr;
    int32_t dataBType = 0;
    if (td)
    {
        Il2CppClass *tdk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(td) : td->klass;
        if (tdk)
        {
            if (FieldInfo *fBt = ArkReflectFindField(tdk, "buildableType", nullptr))
                if (void *a = ArkReflectFieldAddr(fBt, td)) dataBType = *(int32_t *)a;
        }
    }

    int32_t origBType   = InvokeIntGetter(tile, tk, "get_buildableType", dataBType);
    int32_t origAdvMask = InvokeIntGetter(tile, tk, "get_advancedBuildableMask", 1);
    uint8_t origBlk     = InvokeBoolGetter(tile, tk, "get_blockManuallySpawn") ? 1 : 0;
    uint8_t origOvl     = InvokeBoolGetter(tile, tk, "get_enableOverlap") ? 1 : 0;
    uint8_t origOnlyMO  = InvokeBoolGetter(tile, tk, "get_onlyManuallyOverlap") ? 1 : 0;

    TileBackup *b = &g_tileBackups[g_tileBackupCount++];
    b->tile                  = tile;
    b->tileData              = td;
    b->origBuildableType     = origBType;
    b->origAdvMask           = origAdvMask;
    b->origBlockManual       = origBlk;
    b->origOverlap           = origOvl;
    b->origOnlyManualOverlap = origOnlyMO;
    return b;
}

static void SetTileBoolProp(Il2CppObject *tile, Il2CppClass *tk, const char *setterName, const char *fieldName, bool val)
{
    Il2CppApi &api = ir::Api();
    if (MethodInfo *mSet = FindMethodWalk(tk, setterName, 1))
    {
        uint8_t v = val ? 1 : 0;
        void *args[1] = { &v };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mSet, tile, args, &exc);
        if (!exc) return;
    }
    if (FieldInfo *f = ArkReflectFindField(tk, fieldName, nullptr))
        if (void *a = ArkReflectFieldAddr(f, tile)) *(uint8_t *)a = val ? 1 : 0;
}

static void SetTileBuildableAndMask(Il2CppObject *tile, Il2CppClass *tk, Il2CppObject *tileData,
                                    int32_t bType, int32_t advMask)
{
    Il2CppApi &api = ir::Api();
    // 1) 调用原生 Tile.OverwriteBuildableType / OverwriteAdvancedBuildableMask，同步刷新 Tile.m_cachedOptions
    if (MethodInfo *mOwBt = FindMethodWalk(tk, "OverwriteBuildableType", 1))
    {
        int32_t v = bType;
        void *args[1] = { &v };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mOwBt, tile, args, &exc);
    }
    if (MethodInfo *mOwAdv = FindMethodWalk(tk, "OverwriteAdvancedBuildableMask", 1))
    {
        int32_t v = advMask;
        void *args[1] = { &v };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mOwAdv, tile, args, &exc);
    }
    // 2) 同步写入 m_data.buildableType / advancedBuildableMask 作为双保险
    if (tileData)
    {
        Il2CppClass *tdk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(tileData) : tileData->klass;
        if (tdk)
        {
            if (FieldInfo *fBt = ArkReflectFindField(tdk, "buildableType", nullptr))
                if (void *a = ArkReflectFieldAddr(fBt, tileData)) *(int32_t *)a = bType;
            if (FieldInfo *fAm = ArkReflectFindField(tdk, "advancedBuildableMask", nullptr))
                if (void *a = ArkReflectFieldAddr(fAm, tileData)) *(int32_t *)a = advMask;
        }
    }
}

static void ApplyRuleAllTerrain(bool enable, int *outTiles, int *outCards)
{
    if (outTiles) *outTiles = 0;
    if (outCards) *outCards = 0;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *mapObj = GetBattleMapFast(bc);

    int tHit = 0, cHit = 0;
    if (enable)
    {
        if (mapObj)
        {
            Il2CppObject *tiles[512];
            int tn = GetMapTilesFast(mapObj, tiles, 512);
            for (int i = 0; i < tn; ++i)
            {
                Il2CppObject *tile = tiles[i];
                Il2CppClass *tk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(tile) : tile->klass;
                if (!tk || IsProtectedSpecialTile(tile, tk)) continue;

                TileBackup *bk = FindOrRecordTileBackup(tile, tk);
                if (!bk) continue;

                // BuildableType.ALL(3) + AdvancedBuildableMask.DEFAULT(1)（解除深海/红雾/禁区等特殊地形限制）
                SetTileBuildableAndMask(tile, tk, bk->tileData, 3, 1);
                SetTileBoolProp(tile, tk, "set_blockManuallySpawn", "m_blockManuallySpawn", false);
                ++tHit;
            }
        }

        // 同步放宽所有手牌的 BuildCondition（含 m_buildCondition 与 m_originBuildableCondition）
        Il2CppObject *cards[128];
        int cn = bc ? GetBattleCardsFast(bc, cards, 128) : 0;
        for (int i = 0; i < cn; ++i)
        {
            Il2CppObject *card = cards[i];
            if (!card) continue;
            Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(card) : card->klass;
            if (!ck) continue;

            FieldInfo *fCond     = ArkReflectFindField(ck, "m_buildCondition", nullptr);
            FieldInfo *fOrigCond = ArkReflectFindField(ck, "m_originBuildableCondition", nullptr);
            if (!fCond && !fOrigCond) continue;

            FieldInfo *fRef = fCond ? fCond : fOrigCond;
            Il2CppType *ft = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(fRef) : nullptr;
            Il2CppClass *condK = (ft && api.il2cpp_class_from_type) ? api.il2cpp_class_from_type(ft) : nullptr;
            if (!condK) continue;

            // 查找或记录备份
            CardCondBackup *cb = nullptr;
            for (int k = 0; k < g_cardCondBackupCount; ++k)
                if (g_cardCondBackups[k].card == card) { cb = &g_cardCondBackups[k]; break; }
            if (!cb && g_cardCondBackupCount < 128)
            {
                cb = &g_cardCondBackups[g_cardCondBackupCount++];
                memset(cb, 0, sizeof(*cb));
                cb->card = card;
                if (fCond)
                {
                    if (void *sb = ArkReflectFieldAddr(fCond, card))
                    {
                        if (void *p = StructMemberAddr(sb, condK, "buildableType"))              cb->origBuildType      = *(int32_t *)p;
                        if (void *p = StructMemberAddr(sb, condK, "advancedBuildableMask"))      cb->origAdvMask        = *(int32_t *)p;
                        if (void *p = StructMemberAddr(sb, condK, "checkManuallyBuildableType")) cb->origCheckManual    = *(uint8_t *)p;
                        if (void *p = StructMemberAddr(sb, condK, "manuallyBuildableType"))      cb->origManualType     = *(int32_t *)p;
                        if (void *p = StructMemberAddr(sb, condK, "_excludeOccupiedByWalkEnemy"))cb->origExcludeWalk    = *(uint8_t *)p;
                        if (void *p = StructMemberAddr(sb, condK, "excludeNoTargetTile"))        cb->origExcludeNoTarget= *(uint8_t *)p;
                    }
                }
                if (fOrigCond)
                {
                    if (void *sb = ArkReflectFieldAddr(fOrigCond, card))
                        if (void *p = StructMemberAddr(sb, condK, "buildableType")) cb->origOrigBuildType = *(int32_t *)p;
                }
            }

            auto relaxCond = [&](FieldInfo *f) {
                if (!f) return;
                void *sb = ArkReflectFieldAddr(f, card);
                if (!sb) return;
                if (void *p = StructMemberAddr(sb, condK, "buildableType"))               *(int32_t *)p = 3;     // ALL
                if (void *p = StructMemberAddr(sb, condK, "advancedBuildableMask"))       *(int32_t *)p = 0x7FF; // 全特殊地块通行
                if (void *p = StructMemberAddr(sb, condK, "checkManuallyBuildableType"))  *(uint8_t *)p = 0;
                if (void *p = StructMemberAddr(sb, condK, "manuallyBuildableType"))       *(int32_t *)p = 3;
                if (void *p = StructMemberAddr(sb, condK, "_excludeOccupiedByWalkEnemy")) *(uint8_t *)p = 0;
                if (void *p = StructMemberAddr(sb, condK, "excludeNoTargetTile"))         *(uint8_t *)p = 0;
            };
            relaxCond(fCond);
            relaxCond(fOrigCond);
            ++cHit;
        }
        MarkMapBuildableDirty(mapObj);
    }
    else
    {
        // 关闭时精确还原每个地块的原始可部署类型、特殊掩码与手动禁放标记
        for (int i = 0; i < g_tileBackupCount; ++i)
        {
            TileBackup &bk = g_tileBackups[i];
            if (!bk.tile || !IsUnityObjAlive(bk.tile)) continue;
            Il2CppClass *tk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bk.tile) : bk.tile->klass;
            if (tk)
            {
                SetTileBuildableAndMask(bk.tile, tk, bk.tileData, bk.origBuildableType, bk.origAdvMask);
                SetTileBoolProp(bk.tile, tk, "set_blockManuallySpawn", "m_blockManuallySpawn", bk.origBlockManual != 0);
            }
            ++tHit;
        }
        // 还原手牌 BuildCondition
        for (int i = 0; i < g_cardCondBackupCount; ++i)
        {
            CardCondBackup &cb = g_cardCondBackups[i];
            if (!cb.card) continue;
            Il2CppClass *ck = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(cb.card) : cb.card->klass;
            if (!ck) continue;
            FieldInfo *fCond     = ArkReflectFindField(ck, "m_buildCondition", nullptr);
            FieldInfo *fOrigCond = ArkReflectFindField(ck, "m_originBuildableCondition", nullptr);
            FieldInfo *fRef = fCond ? fCond : fOrigCond;
            if (!fRef) continue;
            Il2CppType *ft = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(fRef) : nullptr;
            Il2CppClass *condK = (ft && api.il2cpp_class_from_type) ? api.il2cpp_class_from_type(ft) : nullptr;
            if (!condK) continue;

            if (fCond)
            {
                if (void *sb = ArkReflectFieldAddr(fCond, cb.card))
                {
                    if (void *p = StructMemberAddr(sb, condK, "buildableType"))               *(int32_t *)p = cb.origBuildType;
                    if (void *p = StructMemberAddr(sb, condK, "advancedBuildableMask"))       *(int32_t *)p = cb.origAdvMask;
                    if (void *p = StructMemberAddr(sb, condK, "checkManuallyBuildableType"))  *(uint8_t *)p = cb.origCheckManual;
                    if (void *p = StructMemberAddr(sb, condK, "manuallyBuildableType"))       *(int32_t *)p = cb.origManualType;
                    if (void *p = StructMemberAddr(sb, condK, "_excludeOccupiedByWalkEnemy")) *(uint8_t *)p = cb.origExcludeWalk;
                    if (void *p = StructMemberAddr(sb, condK, "excludeNoTargetTile"))         *(uint8_t *)p = cb.origExcludeNoTarget;
                }
            }
            if (fOrigCond)
            {
                if (void *sb = ArkReflectFieldAddr(fOrigCond, cb.card))
                    if (void *p = StructMemberAddr(sb, condK, "buildableType")) *(int32_t *)p = cb.origOrigBuildType;
            }
            ++cHit;
        }
        g_cardCondBackupCount = 0;
        if (InterlockedCompareExchange(&g_ruleOverlap, 0, 0) == 0)
            g_tileBackupCount = 0;
        MarkMapBuildableDirty(mapObj);
    }

    if (outTiles) *outTiles = tHit;
    if (outCards) *outCards = cHit;
}

static void ApplyRuleOverlap(bool enable, int *outTiles)
{
    if (outTiles) *outTiles = 0;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *mapObj = GetBattleMapFast(bc);
    int tHit = 0;

    if (enable)
    {
        if (mapObj)
        {
            Il2CppObject *tiles[512];
            int tn = GetMapTilesFast(mapObj, tiles, 512);
            for (int i = 0; i < tn; ++i)
            {
                Il2CppObject *tile = tiles[i];
                Il2CppClass *tk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(tile) : tile->klass;
                if (!tk || IsProtectedSpecialTile(tile, tk)) continue;
                FindOrRecordTileBackup(tile, tk);
                SetTileBoolProp(tile, tk, "set_enableOverlap", "m_enableOverlap", true);
                SetTileBoolProp(tile, tk, "set_onlyManuallyOverlap", "m_onlyManuallyOverlap", false);
                ++tHit;
            }
            MarkMapBuildableDirty(mapObj);
        }
    }
    else
    {
        for (int i = 0; i < g_tileBackupCount; ++i)
        {
            TileBackup &bk = g_tileBackups[i];
            if (!bk.tile || !IsUnityObjAlive(bk.tile)) continue;
            Il2CppClass *tk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bk.tile) : bk.tile->klass;
            if (tk)
            {
                SetTileBoolProp(bk.tile, tk, "set_enableOverlap", "m_enableOverlap", bk.origOverlap != 0);
                SetTileBoolProp(bk.tile, tk, "set_onlyManuallyOverlap", "m_onlyManuallyOverlap", bk.origOnlyManualOverlap != 0);
                ++tHit;
            }
        }
        if (InterlockedCompareExchange(&g_ruleAllTerrain, 0, 0) == 0)
            g_tileBackupCount = 0;
        MarkMapBuildableDirty(mapObj);
    }
    if (outTiles) *outTiles = tHit;
}

// 递归收集 RangeSelector（完整覆盖远程 AdvancedSelector + 95% 近战 BlockedOrAdvancedSelector/BlockedAndAdvancedSelector
// + 复合容器 GroupSelector / SequenceGroupSelector / ParallelGroupSelector；由 is_assignable_from 自动排除无射程的纯 BlockedSelector/SelfSelector）
static void CollectRangeSelectorRec(Il2CppObject *sel, Il2CppClass *rsKlass, Il2CppObject **outSels, int maxOut, int *found, int depth)
{
    if (!sel || !rsKlass || !outSels || !found || *found >= maxOut || depth > 3 || !IsUnityObjAlive(sel))
        return;
    Il2CppApi &api = ir::Api();
    Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(sel) : sel->klass;
    if (!sk) return;

    // 关键：必须继承自 RangeSelector（自带 m_range 与 m_targetOptions），
    // 自动安全排除直接继承 TargetSelector 的纯 BlockedSelector / SelfSelector，
    // 同时完整保留所有近战干员的 BlockedOrAdvancedSelector / BlockedAndAdvancedSelector / BlockedOrRandomSelector！
    if (api.il2cpp_class_is_assignable_from && !api.il2cpp_class_is_assignable_from(rsKlass, sk))
        return;

    bool dup = false;
    for (int i = 0; i < *found; ++i)
        if (outSels[i] == sel) { dup = true; break; }
    if (!dup)
        outSels[(*found)++] = sel;

    // 展开复合选择器内部子选择器（GroupSelector / SequenceGroupSelector / ParallelGroupSelector）
    if (FieldInfo *fList = ArkReflectFindField(sk, "_rangeSelectorsList", nullptr))
    {
        if (Il2CppObject *listObj = api.il2cpp_field_get_value_object(fList, sel))
        {
            Il2CppClass *lk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(listObj) : listObj->klass;
            if (lk)
            {
                FieldInfo *fItems = ArkReflectFindField(lk, "_items", nullptr);
                FieldInfo *fSize  = ArkReflectFindField(lk, "_size", nullptr);
                if (fItems && fSize)
                {
                    int32_t sz = 0;
                    if (void *a = ArkReflectFieldAddr(fSize, listObj)) sz = *(int32_t *)a;
                    if (sz > 0 && sz <= 32)
                    {
                        if (Il2CppObject *arr = api.il2cpp_field_get_value_object(fItems, listObj))
                        {
                            int32_t arrLen = *(int32_t *)((char *)arr + 0x18);
                            int32_t take = (sz < arrLen) ? sz : arrLen;
                            Il2CppObject **items = (Il2CppObject **)((char *)arr + 0x20);
                            for (int i = 0; i < take && *found < maxOut; ++i)
                                CollectRangeSelectorRec(items[i], rsKlass, outSels, maxOut, found, depth + 1);
                        }
                    }
                }
            }
        }
    }
    if (FieldInfo *fArr = ArkReflectFindField(sk, "_selectorGroup", nullptr))
    {
        if (Il2CppObject *arr = api.il2cpp_field_get_value_object(fArr, sel))
        {
            int32_t arrLen = *(int32_t *)((char *)arr + 0x18);
            if (arrLen > 0 && arrLen <= 32)
            {
                Il2CppObject **items = (Il2CppObject **)((char *)arr + 0x20);
                for (int i = 0; i < arrLen && *found < maxOut; ++i)
                    CollectRangeSelectorRec(items[i], rsKlass, outSels, maxOut, found, depth + 1);
            }
        }
    }
    static const char *kChildSelFields[] = { "_overrideSelector", "_firstOrderSelector", "_appendOrderSelector" };
    for (int fi = 0; fi < 3 && *found < maxOut; ++fi)
    {
        if (FieldInfo *fc = ArkReflectFindField(sk, kChildSelFields[fi], nullptr))
            CollectRangeSelectorRec(api.il2cpp_field_get_value_object(fc, sel), rsKlass, outSels, maxOut, found, depth + 1);
    }
}

// 零 GC 收集干员身上全部 RangeSelector（普攻 + 形态切换/技能覆写攻击 + 战斗能力 + 技能 + 缓存能力）
static int CollectCharRangeSelectors(Il2CppObject *chr, Il2CppClass *ck, Il2CppObject **outSels, int maxOut)
{
    if (!chr || !ck || !outSels || maxOut <= 0) return 0;
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *rsCe = ArkReflectFindClass("Torappu.Battle.RangeSelector");
    Il2CppClass *rsKlass = rsCe ? rsCe->klass : nullptr;
    if (!rsKlass) return 0;

    int found = 0;
    auto addSel = [&](Il2CppObject *sel) {
        CollectRangeSelectorRec(sel, rsKlass, outSels, maxOut, &found, 0);
    };

    auto addFromTrigger = [&](Il2CppObject *trig) {
        if (!trig) return;
        Il2CppClass *tk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(trig) : trig->klass;
        if (!tk) return;
        if (FieldInfo *fSel = ArkReflectFindField(tk, "m_selector", nullptr))
            addSel(api.il2cpp_field_get_value_object(fSel, trig));
        if (MethodInfo *mGs = FindMethodWalk(tk, "get_selector", 0))
        {
            Il2CppException *e2 = nullptr;
            Il2CppObject *s = api.il2cpp_runtime_invoke(mGs, trig, nullptr, &e2);
            if (!e2 && s) addSel(s);
        }
    };

    auto addFromAbility = [&](Il2CppObject *ab) {
        if (!ab) return;
        Il2CppClass *ak = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(ab) : ab->klass;
        if (!ak) return;
        if (FieldInfo *fSel = ArkReflectFindField(ak, "_selector", nullptr))
            addSel(api.il2cpp_field_get_value_object(fSel, ab));
        if (MethodInfo *mGetSel = FindMethodWalk(ak, "get_selector", 0))
        {
            Il2CppException *exc = nullptr;
            Il2CppObject *s = api.il2cpp_runtime_invoke(mGetSel, ab, nullptr, &exc);
            if (!exc && s) addSel(s);
        }
    };

    // 1) Unit.get_attackTrigger() & m_overrideTrigger
    if (MethodInfo *mTrig = FindMethodWalk(ck, "get_attackTrigger", 0))
    {
        Il2CppException *exc = nullptr;
        Il2CppObject *trig = api.il2cpp_runtime_invoke(mTrig, chr, nullptr, &exc);
        if (!exc && trig) addFromTrigger(trig);
    }
    if (FieldInfo *fOvTrig = ArkReflectFindField(ck, "m_overrideTrigger", nullptr))
        addFromTrigger(api.il2cpp_field_get_value_object(fOvTrig, chr));

    // 2) Unit.get_attack() / get_combat() / m_overrideAttack / m_overrideCombat / Character.m_skill
    static const char *kAbGetters[] = { "get_attack", "get_combat" };
    for (int gi = 0; gi < 2 && found < maxOut; ++gi)
    {
        if (MethodInfo *mAb = FindMethodWalk(ck, kAbGetters[gi], 0))
        {
            Il2CppException *exc = nullptr;
            Il2CppObject *ab = api.il2cpp_runtime_invoke(mAb, chr, nullptr, &exc);
            if (!exc && ab) addFromAbility(ab);
        }
    }
    static const char *kAbFields[] = { "m_overrideAttack", "m_overrideCombat", "m_skill" };
    for (int fi = 0; fi < 3 && found < maxOut; ++fi)
    {
        if (FieldInfo *fAb = ArkReflectFindField(ck, kAbFields[fi], nullptr))
            addFromAbility(api.il2cpp_field_get_value_object(fAb, chr));
    }

    // 3) Entity.m_cachedAbilities (List<Ability>) -> 各技能/天赋的能力选择器
    if (FieldInfo *fCached = ArkReflectFindField(ck, "m_cachedAbilities", nullptr))
    {
        if (Il2CppObject *abList = api.il2cpp_field_get_value_object(fCached, chr))
        {
            Il2CppClass *lk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(abList) : abList->klass;
            if (lk)
            {
                FieldInfo *fItems = ArkReflectFindField(lk, "_items", nullptr);
                FieldInfo *fSize  = ArkReflectFindField(lk, "_size", nullptr);
                if (fItems && fSize)
                {
                    int32_t sz = 0;
                    if (void *a = ArkReflectFieldAddr(fSize, abList)) sz = *(int32_t *)a;
                    if (sz > 0 && sz <= 256)
                    {
                        if (Il2CppObject *arr = api.il2cpp_field_get_value_object(fItems, abList))
                        {
                            int32_t arrLen = *(int32_t *)((char *)arr + 0x18);
                            int32_t take = (sz < arrLen) ? sz : arrLen;
                            if (take > 256) take = 256;
                            Il2CppObject **abItems = (Il2CppObject **)((char *)arr + 0x20);
                            for (int i = 0; i < take && found < maxOut; ++i)
                                addFromAbility(abItems[i]);
                        }
                    }
                }
            }
        }
    }
    return found;
}

static void NotifyUnitRangeChanged(Il2CppObject *chr, Il2CppClass *ck)
{
    Il2CppApi &api = ir::Api();
    if (!chr || !ck) return;
    if (FieldInfo *fDirty = ArkReflectFindField(ck, "m_attackRangeDirty", nullptr))
        if (void *a = ArkReflectFieldAddr(fDirty, chr)) *(uint8_t *)a = 1;
    if (MethodInfo *mChg = FindMethodWalk(ck, "OnAttackRangeChanged", 0))
    {
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mChg, chr, nullptr, &exc);
    }
}

// 全图攻击范围（挂载真实 Unity 托管 GlobalRange 组件，绝不用 il2cpp_object_new 造假 MonoBehaviour！）
static void ApplyRuleGlobalRange(bool enable, const char *filter, int *outChars, int *outSels)
{
    if (outChars) *outChars = 0;
    if (outSels)  *outSels  = 0;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *charCe = ArkReflectFindClass("Torappu.Battle.Character");
    const ArkClassEntry *grCe   = ArkReflectFindClass("Torappu.Battle.GlobalRange");
    if (!bc || !charCe || !charCe->klass || !grCe || !grCe->klass) return;

    int cHit = 0, sHit = 0;
    if (enable)
    {
        if (!api.il2cpp_class_get_type || !api.il2cpp_type_get_object || !api.il2cpp_field_set_value_object)
            return;
        Il2CppObject *grTypeObj = api.il2cpp_type_get_object(api.il2cpp_class_get_type(grCe->klass));
        if (!grTypeObj) return;

        Il2CppObject *chars[256];
        int n = GetBattleCharactersFast(bc, chars, 256);
        FieldInfo *fData = ArkReflectFindField(charCe->klass, "<data>k__BackingField", nullptr);

        for (int i = 0; i < n; ++i)
        {
            Il2CppObject *chr = chars[i];
            if (!chr || !fData || !IsEntityAlive(chr, charCe->klass)) continue;
            char desc[128] = { 0 };
            if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
                continue;

            Il2CppObject *sels[32];
            int sn = CollectCharRangeSelectors(chr, charCe->klass, sels, 32);
            bool charTouched = false;
            for (int j = 0; j < sn; ++j)
            {
                Il2CppObject *sel = sels[j];
                Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(sel) : sel->klass;
                if (!sk) continue;
                FieldInfo *fRange = ArkReflectFindField(sk, "m_range", nullptr);
                if (!fRange) continue;

                Il2CppObject *curRange = api.il2cpp_field_get_value_object(fRange, sel);
                if (curRange)
                {
                    Il2CppClass *rk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(curRange) : curRange->klass;
                    if (rk == grCe->klass && IsUnityObjAlive(curRange))
                        continue; // 已是全图射程，幂等跳过
                }

                // 拿 Selector 所在 GameObject，优先复用已有 GlobalRange 组件，否则 AddComponent(typeof(GlobalRange))
                MethodInfo *mGetGo = FindMethodWalk(sk, "get_gameObject", 0);
                if (!mGetGo) continue;
                Il2CppException *exc = nullptr;
                Il2CppObject *go = api.il2cpp_runtime_invoke(mGetGo, sel, nullptr, &exc);
                if (exc || !go || !IsUnityObjAlive(go)) continue;

                Il2CppClass *gok = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(go) : go->klass;
                if (!gok) continue;

                Il2CppObject *grComp = nullptr;
                if (MethodInfo *mGetComp = FindMethodWithParamType(gok, "GetComponent", 1, "Type"))
                {
                    void *args[1] = { grTypeObj };
                    exc = nullptr;
                    Il2CppObject *c = api.il2cpp_runtime_invoke(mGetComp, go, args, &exc);
                    if (!exc && c && IsUnityObjAlive(c)) grComp = c;
                }
                if (!grComp)
                {
                    if (MethodInfo *mAddComp = FindMethodWithParamType(gok, "AddComponent", 1, "Type"))
                    {
                        void *args[1] = { grTypeObj };
                        exc = nullptr;
                        Il2CppObject *c = api.il2cpp_runtime_invoke(mAddComp, go, args, &exc);
                        if (!exc && c && IsUnityObjAlive(c)) grComp = c;
                    }
                }
                if (!grComp) continue;

                // 记录原射程备份（每个 selector 只记录一次原始 m_range）
                bool alreadyBacked = false;
                for (int b = 0; b < g_rangeBackupCount; ++b)
                    if (g_rangeBackups[b].selector == sel) { alreadyBacked = true; break; }
                if (!alreadyBacked && g_rangeBackupCount < 256)
                {
                    g_rangeBackups[g_rangeBackupCount].selector  = sel;
                    g_rangeBackups[g_rangeBackupCount].origRange = curRange;
                    ++g_rangeBackupCount;
                }

                api.il2cpp_field_set_value_object(sel, fRange, grComp);
                charTouched = true;
                ++sHit;
            }
            if (charTouched)
            {
                NotifyUnitRangeChanged(chr, charCe->klass);
                ++cHit;
            }
        }
    }
    else
    {
        for (int b = 0; b < g_rangeBackupCount; ++b)
        {
            RangeBackup &rb = g_rangeBackups[b];
            if (!rb.selector || !IsUnityObjAlive(rb.selector)) continue;
            Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(rb.selector) : rb.selector->klass;
            if (!sk) continue;
            if (FieldInfo *fRange = ArkReflectFindField(sk, "m_range", nullptr))
            {
                if (api.il2cpp_field_set_value_object)
                    api.il2cpp_field_set_value_object(rb.selector, fRange, rb.origRange);
                ++sHit;
            }
        }
        g_rangeBackupCount = 0;
        // 通知场上所有存活干员刷新攻击范围显示
        Il2CppObject *chars[256];
        int n = GetBattleCharactersFast(bc, chars, 256);
        for (int i = 0; i < n; ++i)
        {
            if (chars[i] && IsEntityAlive(chars[i], charCe->klass))
            {
                NotifyUnitRangeChanged(chars[i], charCe->klass);
                ++cHit;
            }
        }
    }
    if (outChars) *outChars = cHit;
    if (outSels)  *outSels  = sHit;
}

// 照妖镜：剥离场上敌方怪物的隐匿 (INVISIBLE=9) 与迷彩 (CAMOUFLAGE=17) 状态
static int ApplyEnemyRevealStealth(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass || !api.il2cpp_runtime_invoke) return 0;

    Il2CppObject *bc = GetBattleControllerFast();
    Il2CppObject *objs[256];
    int n = bc ? GetBattleEnemiesFast(bc, objs, 256) : 0;
    if (n <= 0) return 0;

    MethodInfo *mRemFlag = FindMethodWalk(ce->klass, "RemoveAllBuffsWithCertainAbnormalFlag", 1);
    if (!mRemFlag) return 0;

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *enm = objs[i];
        if (!enm || !IsEntityAlive(enm, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckEnemyMatches(enm, ce->klass, filter, desc, sizeof(desc))) continue;

        int32_t flagInv  = 9;  // AbnormalFlag.INVISIBLE
        int32_t flagCamo = 17; // AbnormalFlag.CAMOUFLAGE
        void *a1[1] = { &flagInv };
        void *a2[1] = { &flagCamo };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mRemFlag, enm, a1, &exc);
        exc = nullptr;
        api.il2cpp_runtime_invoke(mRemFlag, enm, a2, &exc);
        CopyStrSafe(lastMatched, cap, desc);
        ++hit;
    }
    return hit;
}

// 无视对地/对空判定 + 破隐匿/迷彩（铁律：绝不修改 targetSide / targetCategory，确保奶妈不奶怪、输出不打队友）
static void ApplyRuleIgnoreMotion(bool enable, const char *filter, int *outSels, int *outEnemiesRevealed)
{
    if (outSels) *outSels = 0;
    if (outEnemiesRevealed) *outEnemiesRevealed = 0;
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *charCe = ArkReflectFindClass("Torappu.Battle.Character");
    if (!bc || !charCe || !charCe->klass) return;

    int sHit = 0;
    if (enable)
    {
        Il2CppObject *chars[256];
        int n = GetBattleCharactersFast(bc, chars, 256);
        FieldInfo *fData = ArkReflectFindField(charCe->klass, "<data>k__BackingField", nullptr);

        for (int i = 0; i < n; ++i)
        {
            Il2CppObject *chr = chars[i];
            if (!chr || !fData || !IsEntityAlive(chr, charCe->klass)) continue;
            char desc[128] = { 0 };
            if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
                continue;

            Il2CppObject *sels[32];
            int sn = CollectCharRangeSelectors(chr, charCe->klass, sels, 32);
            for (int j = 0; j < sn; ++j)
            {
                Il2CppObject *sel = sels[j];
                Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(sel) : sel->klass;
                if (!sk) continue;

                FieldInfo *fOpts    = ArkReflectFindField(sk, "m_targetOptions", nullptr);
                FieldInfo *fAdvMot  = ArkReflectFindField(sk, "_targetMotion", nullptr);
                FieldInfo *fAdvIgn  = ArkReflectFindField(sk, "_ignoreMotionMode", nullptr);
                FieldInfo *fAdvCamo = ArkReflectFindField(sk, "_forceIgnoreCamouflage", nullptr);
                if (!fOpts && !fAdvMot) continue;

                Il2CppClass *optK = nullptr;
                void *optBase = nullptr;
                if (fOpts)
                {
                    Il2CppType *ft = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(fOpts) : nullptr;
                    optK = (ft && api.il2cpp_class_from_type) ? api.il2cpp_class_from_type(ft) : nullptr;
                    optBase = ArkReflectFieldAddr(fOpts, sel);
                }

                // 查找或记录原值备份
                MotionBackup *mb = nullptr;
                for (int b = 0; b < g_motionBackupCount; ++b)
                    if (g_motionBackups[b].selector == sel) { mb = &g_motionBackups[b]; break; }
                if (!mb && g_motionBackupCount < 256)
                {
                    mb = &g_motionBackups[g_motionBackupCount++];
                    memset(mb, 0, sizeof(*mb));
                    mb->selector = sel;
                    if (optBase && optK)
                    {
                        if (void *p = StructMemberAddr(optBase, optK, "targetMotion"))          mb->origOptMotion       = *(int32_t *)p;
                        if (void *p = StructMemberAddr(optBase, optK, "ignoreMotionMode"))      mb->origOptIgnoreMotion = *(uint8_t *)p;
                        if (void *p = StructMemberAddr(optBase, optK, "enableAdvancedOptions")) mb->origOptAdv          = *(uint8_t *)p;
                    }
                    if (fAdvMot || fAdvIgn || fAdvCamo)
                    {
                        mb->hasAdvFields = 1;
                        if (fAdvMot)  if (void *a = ArkReflectFieldAddr(fAdvMot, sel))  mb->origAdvMotion          = *(int32_t *)a;
                        if (fAdvIgn)  if (void *a = ArkReflectFieldAddr(fAdvIgn, sel))  mb->origAdvIgnoreMotion    = *(uint8_t *)a;
                        if (fAdvCamo) if (void *a = ArkReflectFieldAddr(fAdvCamo, sel)) mb->origAdvForceIgnoreCamo = *(uint8_t *)a;
                    }
                }

                // 仅修改地/空与反迷彩字段，绝不碰 targetSide / targetCategory
                bool changed = false;
                if (optBase && optK)
                {
                    if (void *p = StructMemberAddr(optBase, optK, "targetMotion"))
                        if (*(int32_t *)p != 3) { *(int32_t *)p = 3; changed = true; } // MotionMask.ALL
                    if (void *p = StructMemberAddr(optBase, optK, "enableAdvancedOptions"))
                        if (*(uint8_t *)p != 1) { *(uint8_t *)p = 1; changed = true; }
                    if (void *p = StructMemberAddr(optBase, optK, "ignoreMotionMode"))
                        if (*(uint8_t *)p != 1) { *(uint8_t *)p = 1; changed = true; }
                }
                if (fAdvMot)
                    if (void *a = ArkReflectFieldAddr(fAdvMot, sel))
                        if (*(int32_t *)a != 3) { *(int32_t *)a = 3; changed = true; }
                if (fAdvIgn)
                    if (void *a = ArkReflectFieldAddr(fAdvIgn, sel))
                        if (*(uint8_t *)a != 1) { *(uint8_t *)a = 1; changed = true; }
                if (fAdvCamo)
                    if (void *a = ArkReflectFieldAddr(fAdvCamo, sel))
                        if (*(uint8_t *)a != 1) { *(uint8_t *)a = 1; changed = true; }
                if (changed) ++sHit;
            }
        }
        char dummy[128] = { 0 };
        int rev = ApplyEnemyRevealStealth("", dummy, sizeof(dummy));
        if (outEnemiesRevealed) *outEnemiesRevealed = rev;
    }
    else
    {
        for (int b = 0; b < g_motionBackupCount; ++b)
        {
            MotionBackup &mb = g_motionBackups[b];
            if (!mb.selector || !IsUnityObjAlive(mb.selector)) continue;
            Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(mb.selector) : mb.selector->klass;
            if (!sk) continue;
            if (FieldInfo *fOpts = ArkReflectFindField(sk, "m_targetOptions", nullptr))
            {
                Il2CppType *ft = api.il2cpp_field_get_type ? api.il2cpp_field_get_type(fOpts) : nullptr;
                Il2CppClass *optK = (ft && api.il2cpp_class_from_type) ? api.il2cpp_class_from_type(ft) : nullptr;
                void *optBase = ArkReflectFieldAddr(fOpts, mb.selector);
                if (optBase && optK)
                {
                    if (void *p = StructMemberAddr(optBase, optK, "targetMotion"))          *(int32_t *)p = mb.origOptMotion;
                    if (void *p = StructMemberAddr(optBase, optK, "ignoreMotionMode"))      *(uint8_t *)p = mb.origOptIgnoreMotion;
                    if (void *p = StructMemberAddr(optBase, optK, "enableAdvancedOptions")) *(uint8_t *)p = mb.origOptAdv;
                }
            }
            if (mb.hasAdvFields)
            {
                if (FieldInfo *f = ArkReflectFindField(sk, "_targetMotion", nullptr))
                    if (void *a = ArkReflectFieldAddr(f, mb.selector)) *(int32_t *)a = mb.origAdvMotion;
                if (FieldInfo *f = ArkReflectFindField(sk, "_ignoreMotionMode", nullptr))
                    if (void *a = ArkReflectFieldAddr(f, mb.selector)) *(uint8_t *)a = mb.origAdvIgnoreMotion;
                if (FieldInfo *f = ArkReflectFindField(sk, "_forceIgnoreCamouflage", nullptr))
                    if (void *a = ArkReflectFieldAddr(f, mb.selector)) *(uint8_t *)a = mb.origAdvForceIgnoreCamo;
            }
            ++sHit;
        }
        g_motionBackupCount = 0;
    }
    if (outSels) *outSels = sHit;
}

// 清除单个我方干员身上的常见负面控制异常（眩晕/沉默/冻结/寒冷/束缚/恐惧/麻痹/沉睡）
static void ClearCharNegativeAbnormals(Il2CppObject *chr, Il2CppClass *ck)
{
    Il2CppApi &api = ir::Api();
    if (!chr || !ck || !api.il2cpp_runtime_invoke) return;
    MethodInfo *mRem = FindMethodWalk(ck, "RemoveAllBuffsWithCertainAbnormalFlag", 1);
    if (!mRem) return;
    static const int32_t kNegFlags[] = { 0, 12, 16, 23, 26, 33, 39, 43 };
    for (int i = 0; i < 8; ++i)
    {
        int32_t f = kNegFlags[i];
        void *args[1] = { &f };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mRem, chr, args, &exc);
    }
}

// 泰拉霸体绝缘体：高异常抗性 + 100% 元素伤害/损伤抗性 + 清除全部负面控制
static int ApplyCharImmune(const char *filter, bool writeAttrs, char *lastMatched, int cap)
{
    int hit = 0;
    if (writeAttrs)
    {
        int sk = 0;
        hit += ApplyCharacterAttribute(filter, 26 /* ONE_MINUS_STATUS_RESISTANCE */, 0.001f, lastMatched, cap, nullptr, &sk);
        ApplyCharacterAttribute(filter, 31 /* EP_DAMAGE_RESISTANCE */, 100.0f, lastMatched, cap);
        ApplyCharacterAttribute(filter, 32 /* EP_RESISTANCE */,        100.0f, lastMatched, cap);
        if (hit == 0 && sk > 0) hit = sk;
    }

    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!bc || !ce || !ce->klass) return hit;

    Il2CppObject *chars[256];
    int n = GetBattleCharactersFast(bc, chars, 256);
    FieldInfo *fData = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    int cleaned = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *chr = chars[i];
        if (!chr || !fData || !IsEntityAlive(chr, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
            continue;
        ClearCharNegativeAbnormals(chr, ce->klass);
        CopyStrSafe(lastMatched, cap, desc);
        ++cleaned;
    }
    return (cleaned > hit) ? cleaned : hit;
}

// 全额退款不粘锅：撤退 100% 返还费用
static int ApplyCharFullRefund(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!bc || !ce || !ce->klass) return 0;

    Il2CppObject *chars[256];
    int n = GetBattleCharactersFast(bc, chars, 256);
    FieldInfo *fData  = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    FieldInfo *fRat1  = ArkReflectFindField(ce->klass, "_withdrawCostRecoverRatio", nullptr);
    FieldInfo *fRat2  = ArkReflectFindField(ce->klass, "_maxWithdrawCostRatioOfRawCost", nullptr);
    FieldInfo *fRatM  = ArkReflectFindField(ce->klass, "m_withdrawCostRecoverRatio", nullptr);
    FieldInfo *fLimM  = ArkReflectFindField(ce->klass, "m_limitMaxWithdrawCostByDeployUse", nullptr);
    MethodInfo *mSetR = FindMethodWalk(ce->klass, "SetWithdrawCostRecoverRatio", 3);

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *chr = chars[i];
        if (!chr || !fData || !IsEntityAlive(chr, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
            continue;

        bool needWrite = false;
        if (fRatM)
        {
            if (void *a = ArkReflectFieldAddr(fRatM, chr))
                if (fabsf(*(float *)a - 1.0f) > 0.01f) { *(float *)a = 1.0f; needWrite = true; }
        }
        if (fRat1)
        {
            if (void *a = ArkReflectFieldAddr(fRat1, chr))
                if (fabsf(*(float *)a - 1.0f) > 0.01f) { *(float *)a = 1.0f; needWrite = true; }
        }
        if (fRat2)
        {
            if (void *a = ArkReflectFieldAddr(fRat2, chr))
                if (fabsf(*(float *)a - 1.0f) > 0.01f) { *(float *)a = 1.0f; needWrite = true; }
        }
        if (fLimM)
        {
            if (void *a = ArkReflectFieldAddr(fLimM, chr))
                if (*(uint8_t *)a != 0) { *(uint8_t *)a = 0; needWrite = true; }
        }
        if (mSetR && needWrite)
        {
            float ratio = 1.0f;
            uint8_t isReset = 0;
            uint8_t limitByDeploy = 0;
            void *args[3] = { &ratio, &isReset, &limitByDeploy };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetR, chr, args, &exc);
        }
        if (needWrite)
        {
            CopyStrSafe(lastMatched, cap, desc);
            ++hit;
        }
    }
    return hit;
}

// ============================================================================
// 持续自动锁定驱动（100% 运行在 Unity 主线程顶层消息泵 + 幂等跳过已满状态）
// ============================================================================
static void PumpPersistentLocks()
{
    if (InterlockedCompareExchange(&g_battlePhase, 0, 0) != PHASE_PLAYING)
        return;

    const bool lockAmmo   = InterlockedCompareExchange(&g_lockInfAmmo, 0, 0) != 0;
    const bool lockSp     = InterlockedCompareExchange(&g_lockInfSp, 0, 0) != 0;
    const bool lockHp     = InterlockedCompareExchange(&g_lockGodHp, 0, 0) != 0;
    const bool lockImmune = InterlockedCompareExchange(&g_lockImmune, 0, 0) != 0;

    if (!lockAmmo && !lockSp && !lockHp && !lockImmune)
        return;

    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    if (!bc) return;

    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!ce || !ce->klass) return;

    Il2CppObject *chars[256];
    int n = GetBattleCharactersFast(bc, chars, 256);
    if (n <= 0) return;

    FieldInfo *fData  = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    FieldInfo *fAttr  = ArkReflectFindField(ce->klass, "m_attributes", nullptr);
    FieldInfo *fHp    = ArkReflectFindField(ce->klass, "m_hp", nullptr);
    FieldInfo *fSp    = ArkReflectFindField(ce->klass, "m_sp", nullptr);
    FieldInfo *fMaxSp = ArkReflectFindField(ce->klass, "<maxSp>k__BackingField", nullptr);

    char targetFilter[128];
    CopyStrSafe(targetFilter, sizeof(targetFilter), g_selectedCharId);

    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *chr = chars[i];
        if (!chr || !fData || !IsEntityAlive(chr, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), targetFilter, desc, sizeof(desc)))
            continue;

        if (lockHp)
        {
            float maxHp = 0.0f;
            float curHp = 0.0f;
            if (fAttr)
            {
                if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, chr))
                    maxHp = ReadAttributeOnObj(attrObj, 0);
            }
            if (fHp)
            {
                if (void *a = ArkReflectFieldAddr(fHp, chr))
                    curHp = (float)((double)(*(int64_t *)a) / 4294967296.0);
            }
            // 仅在血量确实低于上限时才回血，避免每帧触发无意义的血条 UI 重绘
            if (maxHp > 0.0f && curHp < maxHp * 0.995f)
                SetEntityCurrentHp(chr, ce->klass, maxHp);
        }
        if (lockSp)
        {
            float curSp = 0.0f;
            float maxSp = 100.0f;
            if (fSp)
            {
                if (void *a = ArkReflectFieldAddr(fSp, chr))
                    curSp = ReadObscuredFpSlot(a);
            }
            if (fMaxSp)
            {
                if (void *a = ArkReflectFieldAddr(fMaxSp, chr))
                {
                    int32_t m = *(int32_t *)a;
                    if (m > 0) maxSp = (float)m;
                }
            }
            if (curSp < maxSp - 0.1f)
                FillEntitySp(chr, ce->klass);
        }
        if (lockAmmo)
        {
            Il2CppObject *myCnts[8];
            int k = FindCharAmmoCounters(chr, ce->klass, myCnts, 8, nullptr, 0);
            for (int j = 0; j < k; ++j)
            {
                Il2CppClass *cc = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(myCnts[j]) : myCnts[j]->klass;
                if (!cc) continue;
                if (FieldInfo *fEvt = ArkReflectFindField(cc, "m_eventCount", nullptr))
                    if (void *a = ArkReflectFieldAddr(fEvt, myCnts[j]))
                        if (*(int32_t *)a != 0) *(int32_t *)a = 0;
                if (FieldInfo *fNot = ArkReflectFindField(cc, "m_notCountNext", nullptr))
                    if (void *a = ArkReflectFieldAddr(fNot, myCnts[j]))
                        if (*(uint8_t *)a != 1) *(uint8_t *)a = 1;
                if (FieldInfo *fRdy = ArkReflectFindField(cc, "m_readyToReset", nullptr))
                    if (void *a = ArkReflectFieldAddr(fRdy, myCnts[j]))
                        if (*(uint8_t *)a != 0) *(uint8_t *)a = 0;
            }
        }
        if (lockImmune)
        {
            // 对新部署的干员补写抵抗属性，并持续清空眩晕/冰冻/神经损伤等控制效果
            if (fAttr)
            {
                if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, chr))
                {
                    if (ReadAttributeOnObj(attrObj, 26) > 0.01f)
                        OverwriteAttributeOnObj(attrObj, 26, 0.001f);
                    if (ReadAttributeOnObj(attrObj, 31) < 99.0f)
                        OverwriteAttributeOnObj(attrObj, 31, 100.0f);
                    if (ReadAttributeOnObj(attrObj, 32) < 99.0f)
                        OverwriteAttributeOnObj(attrObj, 32, 100.0f);
                }
            }
            ClearCharNegativeAbnormals(chr, ce->klass);
        }
    }
}

// ============================================================================
// Unity 主线程直调：动态刷怪 & 战斗倍速（直接由 Mod_OnMainPump 在主线程调用）
// ============================================================================
static void ExecuteSpawnEnemyOnMainThread(const char *enemyKey, int routeIdx, int count, bool applyCustom)
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    int32_t bcState = 0, bcRes = 0;
    bool loaded = false;
    if (bc) ReadBattleControllerMeta(bc, &bcState, nullptr, nullptr, 0, nullptr, &bcRes, &loaded);
    if (!bc || bcState != 2 || bcRes != 0 || !loaded)
    {
        SetFeedback(FB_WARN, "刷怪已取消：当前战斗已结束或尚未开始");
        return;
    }

    // 场上怪物数量安全上限保护（防刷爆 Unity HUD / 对象池引发闪退）
    Il2CppObject *liveEnms[256];
    int liveNow = GetBattleEnemiesFast(bc, liveEnms, 256);
    if (liveNow >= 60)
    {
        SetFeedback(FB_WARN, "刷怪已拦截：当前场上已有 %d 只怪物（安全上限 60），请先清理后再刷", liveNow);
        return;
    }
    int maxAllowed = 60 - (liveNow > 0 ? liveNow : 0);
    int total = (count < 1) ? 1 : (count > 20 ? 20 : count);
    if (total > maxAllowed) total = maxAllowed;

    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    FieldInfo *fLvl = ArkReflectFindField(bcKlass, "m_levelData", nullptr);
    MethodInfo *mGetSched = api.il2cpp_class_get_method_from_name(bcKlass, "get_scheduler", 0);
    if (!fLvl || !mGetSched)
    {
        SetFeedback(FB_ERR, "刷怪失败：找不到 m_levelData 或 get_scheduler");
        return;
    }

    Il2CppObject *lvlObj = api.il2cpp_field_get_value_object(fLvl, bc);
    Il2CppException *exc = nullptr;
    Il2CppObject *schedObj = api.il2cpp_runtime_invoke(mGetSched, bc, nullptr, &exc);
    if (!lvlObj || !schedObj || exc)
    {
        SetFeedback(FB_WARN, "刷怪失败：Scheduler 或 LevelData 为空");
        return;
    }

    Il2CppClass *lk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(lvlObj) : lvlObj->klass;
    Il2CppClass *sk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(schedObj) : schedObj->klass;

    Il2CppObject *routeDataObj = nullptr;
    int usedRouteIdx = 0;
    if (FieldInfo *fRoutes = ArkReflectFindField(lk, "routes", nullptr))
    {
        if (Il2CppObject *rArr = api.il2cpp_field_get_value_object(fRoutes, lvlObj))
        {
            int32_t rLen = *(int32_t *)((char *)rArr + 0x18);
            if (rLen > 0)
            {
                Il2CppObject **rItems = (Il2CppObject **)((char *)rArr + 0x20);
                int startIdx = (routeIdx >= 0) ? (routeIdx % rLen) : 0;
                for (int step = 0; step < rLen; ++step)
                {
                    int idx = (startIdx + step) % rLen;
                    if (rItems[idx])
                    {
                        routeDataObj = rItems[idx];
                        usedRouteIdx = idx;
                        break;
                    }
                }
            }
        }
    }

    MethodInfo *mGenRoute = api.il2cpp_class_get_method_from_name(sk, "GenerateRuntimeRoute", 1);
    MethodInfo *mSpawn = api.il2cpp_class_get_method_from_name(sk, "SpawnEnemyWithRoute", 8);
    if (!routeDataObj || !mGenRoute || !mSpawn || !api.il2cpp_string_new)
    {
        SetFeedback(FB_ERR, "刷怪失败：找不到合法路线或 SpawnEnemyWithRoute 方法");
        return;
    }

    Il2CppString *keyStr = api.il2cpp_string_new(enemyKey);
    int spawned = 0;
    for (int i = 0; i < total; ++i)
    {
        void *rArgs[1] = { routeDataObj };
        exc = nullptr;
        Il2CppObject *routeObj = api.il2cpp_runtime_invoke(mGenRoute, schedObj, rArgs, &exc);
        if (!routeObj || exc) continue;

        uint8_t dontBlockWave      = 1;
        uint8_t unharmful          = 1;
        uint8_t alwaysCountKilled  = 0;
        uint8_t isSummon           = 1;
        uint8_t managedByScheduler = 0;
        uint8_t disableBornColor   = 0;
        void *sArgs[8] = {
            keyStr, routeObj,
            &dontBlockWave, &unharmful, &alwaysCountKilled,
            &isSummon, &managedByScheduler, &disableBornColor
        };
        exc = nullptr;
        Il2CppObject *newEnemy = api.il2cpp_runtime_invoke(mSpawn, schedObj, sArgs, &exc);
        if (newEnemy && !exc)
        {
            ++spawned;
            if (applyCustom)
            {
                Il2CppClass *ek = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(newEnemy) : newEnemy->klass;
                if (FieldInfo *fAttr = ArkReflectFindField(ek, "m_attributes", nullptr))
                {
                    if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, newEnemy))
                    {
                        OverwriteAttributeOnObj(attrObj, 0, g_enemyMaxHp);
                        OverwriteAttributeOnObj(attrObj, 1, g_enemyAtk);
                        OverwriteAttributeOnObj(attrObj, 2, g_enemyDef);
                        OverwriteAttributeOnObj(attrObj, 3, g_enemyMagicRes);
                        OverwriteAttributeOnObj(attrObj, 6, g_enemyMoveSpd);
                        SetEntityCurrentHp(newEnemy, ek, g_enemyMaxHp);
                    }
                }
            }
        }
    }

    if (spawned > 0)
        SetFeedback(FB_OK, "时空裂缝开启！路线 #%d 凭空空投 %d/%d 只 \"%s\"%s",
                    usedRouteIdx, spawned, total, enemyKey,
                    applyCustom ? "（已顺手魔改属性）" : "");
    else
        SetFeedback(FB_WARN, "召唤哑火：\"%s\" 没在本关预加载名单里", enemyKey);
}

static void ExecuteBattleSpeedOnMainThread(int mode, int speedLevel, float timeScale, bool forceApply = false)
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    int32_t bcState = 0, bcRes = 0;
    bool loaded = false;
    if (bc) ReadBattleControllerMeta(bc, &bcState, nullptr, nullptr, 0, nullptr, &bcRes, &loaded);
    if (!bc || bcState != 2 || bcRes != 0 || !loaded)
    {
        SetFeedback(FB_WARN, "时空扭曲失败：还没开打或已经收工啦");
        return;
    }

    // 幂等防重：已是相同档位则跳过
    if (!forceApply)
    {
        if (mode == 0 && g_applied.speedMode == 0 && g_applied.speedLevel == speedLevel)
        {
            SetFeedback(FB_INFO, "时间流速已是当前档位，咱不瞎折腾");
            return;
        }
        if (mode == 1 && g_applied.speedMode == 1 && fabsf(g_applied.timeScale - timeScale) < 0.02f)
        {
            SetFeedback(FB_INFO, "当前已是 %.2fx 自定义倍速，无需重复施法", timeScale);
            return;
        }
    }

    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    bool ok = false;
    if (mode == 0)
    {
        if (MethodInfo *mSetLvl = api.il2cpp_class_get_method_from_name(bcKlass, "set_speedLevel", 1))
        {
            int32_t lvl = speedLevel;
            void *args[1] = { &lvl };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetLvl, bc, args, &exc);
            ok = (exc == nullptr);
        }
        const char *lvlNames[4] = { "子弹时间 (0.5x)", "凡人流速 (1x)", "二倍快进 (2x)", "光速起飞 (3x 极速)" };
        const char *ln = (speedLevel >= 0 && speedLevel < 4) ? lvlNames[speedLevel] : "神秘档位";
        if (ok)
        {
            g_applied.speedMode  = 0;
            g_applied.speedLevel = speedLevel;
            g_applied.timeScale  = (speedLevel == 0) ? 0.5f : (speedLevel == 1 ? 1.0f : (speedLevel == 2 ? 2.0f : 3.0f));
            SetFeedback(FB_OK, "时间法则已改写 -> %s", ln);
        }
        else
            SetFeedback(FB_ERR, "时间法则改写翻车（set_speedLevel 异常）");
    }
    else
    {
        if (MethodInfo *mSetTs = api.il2cpp_class_get_method_from_name(bcKlass, "set_timeScale", 1))
        {
            float ts = timeScale;
            void *args[1] = { &ts };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetTs, bc, args, &exc);
            ok = (exc == nullptr);
        }
        if (ok)
        {
            g_applied.speedMode = 1;
            g_applied.timeScale = timeScale;
            SetFeedback(FB_OK, "时空曲率已锁定为 %.2fx 倍速！", timeScale);
        }
        else
            SetFeedback(FB_ERR, "自定义时间倍率设置失败");
    }
}

// ============================================================================
// 请求消费主循环（100% 在 Unity 主线程顶层消息泵执行，杜绝一切跨线程崩溃！）
// ============================================================================
static void ConsumeBattleRequestOnMainThread()
{
    LONG kind = InterlockedExchange(&g_req.kind, REQ_NONE);
    if (kind == REQ_NONE) return;

    char matched[128] = { 0 };
    bool needRefreshSnap = true;

    // 统一阶段守卫：非战斗进行中时拦截一切战斗修改请求
    const BattlePhase curPhase = (BattlePhase)InterlockedCompareExchange(&g_battlePhase, 0, 0);
    if (kind != REQ_SCAN_ROSTER && kind != REQ_RESET_SESSION && curPhase != PHASE_PLAYING)
    {
        if (curPhase == PHASE_LOBBY)
            SetFeedback(FB_WARN, "别急！你还在基地大厅摸鱼呢，进关卡才能改写物理法则");
        else if (curPhase == PHASE_LOADING)
            SetFeedback(FB_WARN, "干员还在空投舱里下坠呢，等落地进入战场再按！");
        else
            SetFeedback(FB_WARN, "本局已经大捷收工啦，为了零闪退禁止结算期写入");
        InterlockedIncrement(&g_reqDone);
        return;
    }

    switch (kind)
    {
    case REQ_SCAN_ROSTER:
        PollBattleSessionGuard();
        RefreshBattleSnapshot();
        SetFeedback(g_battlePhase == PHASE_PLAYING ? FB_OK : FB_WARN,
                    g_battlePhase == PHASE_PLAYING
                        ? "雷达扫描完毕：罗德岛编队 %d 人 | 场上倒霉蛋 %d 只 (%d 种)"
                        : "当前还没进战场（阶段=%ld），随便开一关立刻自动锁定目标",
                    g_stageCharCount, g_liveEnemyTotal, g_liveEnemyCount, (long)g_battlePhase);
        needRefreshSnap = false;
        break;

    case REQ_RESET_SESSION:
    {
        // 若仍在战斗中，先还原已改写的地形/叠放/射程/地空规则，再清空本地状态
        if (curPhase == PHASE_PLAYING)
        {
            if (InterlockedCompareExchange(&g_ruleAllTerrain, 0, 0) != 0)   ApplyRuleAllTerrain(false, nullptr, nullptr);
            if (InterlockedCompareExchange(&g_ruleOverlap, 0, 0) != 0)      ApplyRuleOverlap(false, nullptr);
            if (InterlockedCompareExchange(&g_ruleGlobalRange, 0, 0) != 0)  ApplyRuleGlobalRange(false, "", nullptr, nullptr);
            if (InterlockedCompareExchange(&g_ruleIgnoreMotion, 0, 0) != 0) ApplyRuleIgnoreMotion(false, "", nullptr, nullptr);
        }
        ResetPerBattleState(true);
        if (curPhase == PHASE_PLAYING)
        {
            ExecuteBattleSpeedOnMainThread(0, 1, 1.0f, true);
            RefreshBattleSnapshot();
        }
        SetFeedback(FB_OK, "物理法则大赦天下！全部锁定/违章规则已关闭还原，恢复 1x 凡人模式");
        needRefreshSnap = false;
        break;
    }

    case REQ_MOD_CARD_COST:
    {
        int skipped = 0;
        int cnt = ApplyCardCostDelta(g_req.text, (int32_t)g_req.val, matched, sizeof(matched), &skipped);
        if (cnt > 0)
        {
            g_applied.zeroCost = true;
            SetFeedback(FB_OK, "白嫖成功！%d 张手牌 (%s) 已设为 0 费空降%s",
                        cnt, matched[0] ? matched : "匹配干员",
                        skipped > 0 ? "（已是 0 费的自动跳过）" : "");
        }
        else if (skipped > 0)
        {
            g_applied.zeroCost = true;
            SetFeedback(FB_INFO, "这 %d 张手牌早就是 0 费白嫖价了，无需重复砍价", skipped);
        }
        else
            SetFeedback(FB_WARN, "手牌里翻了个底朝天也没找到 \"%s\"", g_req.text);
        break;
    }

    case REQ_MOD_CARD_RESPAWN:
    {
        int skipped = 0;
        int cnt = ApplyCardResetRespawn(g_req.text, matched, sizeof(matched), &skipped);
        if (cnt > 0 || skipped > 0)
        {
            g_applied.zeroRespawn = true;
            if (cnt > 0)
                SetFeedback(FB_OK, "医学奇迹！%d 张手牌已开启 1 秒光速诈尸返场并秒清冷却", cnt);
            else
                SetFeedback(FB_INFO, "这 %d 张手牌已是光速重下体质（顺手把当前冷却清零啦）", skipped);
        }
        else
            SetFeedback(FB_WARN, "手牌里没找到匹配 \"%s\" 的干员", g_req.text);
        break;
    }

    case REQ_MOD_DEPLOY_LIMIT:
    {
        bool alreadyBoosted = false;
        int cnt = ApplyFreeDeployLimit(g_req.text, &alreadyBoosted);
        if (cnt > 0 || !alreadyBoosted)
        {
            g_applied.deployLimit = true;
            SetFeedback(FB_OK, alreadyBoosted
                        ? "泰拉人口扩容补票：%d 张新手牌不占位置（上限 +99 已生效，防叠加）"
                        : "泰拉人口大爆炸！%d 张手牌不占部署位 + 战场上限怒加 +99", cnt);
        }
        else
        {
            SetFeedback(FB_INFO, "人口上限早就炸到 +99 啦，不用再叠罗汉式加仓");
        }
        break;
    }

    case REQ_MOD_CHAR_ATTR:
    {
        float beforeVal = 0.0f;
        int skipped = 0;
        int cnt = ApplyCharacterAttribute(g_req.text, g_req.attrType, g_req.val, matched, sizeof(matched), &beforeVal, &skipped);
        const char *attrName =
            (g_req.attrType == 7)  ? "攻速" :
            (g_req.attrType == 8)  ? "攻击间隔" :
            (g_req.attrType == 1)  ? "攻击力" :
            (g_req.attrType == 0)  ? "最大生命" :
            (g_req.attrType == 2)  ? "防御力" :
            (g_req.attrType == 3)  ? "法术抗性" :
            (g_req.attrType == 5)  ? "阻挡数" :
            (g_req.attrType == 14) ? "每秒技力" :
            (g_req.attrType == 20) ? "嘲讽值" :
            (g_req.attrType == 24) ? "推拉力度" : "神秘属性";
        if (cnt > 0)
        {
            if (g_req.attrType >= 0 && g_req.attrType < 64)
                g_applied.charAttrMask |= (1ULL << g_req.attrType);
            SetFeedback(FB_OK, "神兵锻造完成！%d 名干员 (%s) %s：%.2f -> %.2f",
                        cnt, matched[0] ? matched : "匹配干员", attrName, beforeVal, g_req.val);
        }
        else if (skipped > 0)
        {
            if (g_req.attrType >= 0 && g_req.attrType < 64)
                g_applied.charAttrMask |= (1ULL << g_req.attrType);
            SetFeedback(FB_INFO, "场上 %d 名干员的%s早就魔改成 %.2f 啦，无需返工", skipped, attrName, g_req.val);
        }
        else
            SetFeedback(FB_WARN, "场上还没见着 \"%s\"（已提前灌魔到手牌，下场即巅峰！）",
                        g_req.text[0] ? g_req.text : "我方干员");
        break;
    }

    case REQ_MOD_CHAR_FULL_HPSP:
    {
        int cnt = ApplyCharacterFullHpSp(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "大圣奶满！场上 %d 名干员 (%s) 瞬间满血满技力原地复活",
                        cnt, matched[0] ? matched : "全体");
        else
            SetFeedback(FB_WARN, "场上空空如也，先把干员摆上去再奶吧！");
        break;
    }

    case REQ_MOD_CHAR_AMMO:
    {
        int matchedChars = 0;
        int32_t bR = 0, bM = 0, aR = 0, aM = 0;
        bool changeMax = (g_req.extraInt1 != 0);
        int cnt = ApplyCharacterAmmo(g_req.text, (int32_t)g_req.val, changeMax,
                                     &matchedChars, matched, sizeof(matched), &bR, &bM, &aR, &aM);
        if (cnt > 0)
        {
            g_applied.ammoBoosted = true;
            SetFeedback(FB_OK, "无限弹匣扩容成功！%s 子弹 %d/%d -> %d/%d（命中 %d 处）",
                        matched[0] ? matched : "匹配干员", bR, bM, aR, aM, cnt);
        }
        else if (matchedChars == 0)
            SetFeedback(FB_WARN, "场上还没看到目标干员，先请 TA 上台表演！");
        else
            SetFeedback(FB_WARN, "场上 %d 名干员当前没亮弹药条（需先开弹药技能再扩弹匣）", matchedChars);
        break;
    }

    case REQ_MOD_ANGEL_PRESET:
    {
        int sk1 = 0, sk2 = 0;
        bool limBoosted = false;
        int c1 = ApplyCardCostDelta(g_req.text, -99, matched, sizeof(matched), &sk1);
        ApplyCardResetRespawn(g_req.text, matched, sizeof(matched));
        ApplyFreeDeployLimit(g_req.text, &limBoosted);
        ApplyBattleDp(99);
        int c2 = ApplyCharacterAttribute(g_req.text, 7,  g_charAtkSpeed,    matched, sizeof(matched), nullptr, &sk2);
        ApplyCharacterAttribute(g_req.text,          8,  g_charBaseAtkTime, matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          1,  g_charAtk,         matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          0,  g_charMaxHp,       matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          14, g_charSpRecov,     matched, sizeof(matched));
        ApplyCharacterFullHpSp(g_req.text, matched, sizeof(matched));
        int ammoDummy = 0;
        int c3 = ApplyCharacterAmmo(g_req.text, g_charAmmoTarget, true, &ammoDummy, matched, sizeof(matched),
                                    nullptr, nullptr, nullptr, nullptr);
        if (c1 > 0 || sk1 > 0 || c2 > 0 || sk2 > 0)
        {
            g_applied.zeroCost     = true;
            g_applied.zeroRespawn  = true;
            g_applied.deployLimit  = true;
            g_applied.overload     = true;
            g_applied.charAttrMask |= (1ULL << 0) | (1ULL << 1) | (1ULL << 7) | (1ULL << 8) | (1ULL << 14);
            if (c3 > 0) g_applied.ammoBoosted = true;
            SetFeedback(FB_OK, "地爆天星超载机枪已轰鸣！新改手牌=%d | 新改场上=%d | 弹匣=%d | DP=99",
                        c1, c2, c3);
        }
        else
            SetFeedback(FB_WARN, "超载哑火：请确认目标干员已带进本局编队");
        break;
    }

    case REQ_MOD_BATTLE_DP:
    {
        bool already = false;
        if (ApplyBattleDp((int32_t)g_req.val, &already))
            SetFeedback(already ? FB_INFO : FB_OK, already
                        ? "兜里早就揣着 %d 点巨款啦，印钞机歇口气"
                        : "罗德岛印钞机启动！战场费用飙至 %d（上限 999）", (int)g_req.val);
        else
            SetFeedback(FB_WARN, "未找到 BattleController");
        break;
    }

    case REQ_MOD_BATTLE_LIFE:
    {
        bool already = false;
        if (ApplyBattleLifePoint((int32_t)g_req.val, &already))
            SetFeedback(already ? FB_INFO : FB_OK, already
                        ? "蓝门早就被钛合金焊成 %d 血啦，苍蝇都飞不进去"
                        : "蓝门已用钛合金焊死！目标生命直冲 %d 点", (int)g_req.val);
        else
            SetFeedback(FB_WARN, "未找到 BattleController");
        break;
    }

    case REQ_MOD_BATTLE_SPEED:
        ExecuteBattleSpeedOnMainThread(g_req.extraInt1, g_req.extraInt2, g_req.val, false);
        needRefreshSnap = false;
        break;

    case REQ_TOGGLE_ALL_TERRAIN:
    {
        bool wantOn = (InterlockedCompareExchange(&g_ruleAllTerrain, 0, 0) == 0);
        InterlockedExchange(&g_ruleAllTerrain, wantOn ? 1 : 0);
        int tCnt = 0, cCnt = 0;
        ApplyRuleAllTerrain(wantOn, &tCnt, &cCnt);
        if (wantOn)
            SetFeedback(FB_OK, "违章建筑许可证到手！已放开 %d 块地块 + %d 张手牌（高台/地面/禁区随便放，红蓝门已保护）",
                        tCnt, cCnt);
        else
            SetFeedback(FB_INFO, "违章建筑已拆除！已恢复 %d 块原始地形与 %d 张手牌放置限制", tCnt, cCnt);
        needRefreshSnap = false;
        break;
    }

    case REQ_TOGGLE_OVERLAP:
    {
        bool wantOn = (InterlockedCompareExchange(&g_ruleOverlap, 0, 0) == 0);
        InterlockedExchange(&g_ruleOverlap, wantOn ? 1 : 0);
        int tCnt = 0;
        ApplyRuleOverlap(wantOn, &tCnt);
        if (wantOn)
            SetFeedback(FB_OK, "千手观音叠罗汉已激活！%d 块地块允许同格无限叠放干员", tCnt);
        else
            SetFeedback(FB_INFO, "叠罗汉已解散：%d 块地块恢复单格单人制", tCnt);
        needRefreshSnap = false;
        break;
    }

    case REQ_TOGGLE_GLOBAL_RANGE:
    {
        bool wantOn = (InterlockedCompareExchange(&g_ruleGlobalRange, 0, 0) == 0);
        InterlockedExchange(&g_ruleGlobalRange, wantOn ? 1 : 0);
        int cCnt = 0, sCnt = 0;
        ApplyRuleGlobalRange(wantOn, g_selectedCharId, &cCnt, &sCnt);
        if (wantOn)
            SetFeedback(FB_OK, cCnt > 0
                        ? "洲际导弹就位！%d 名干员 (%d 处选择器) 已挂载全图射程（新下干员自动继承）"
                        : "洲际导弹全图狙已待命：现在下场的干员将自动获得全地图攻击范围！", cCnt, sCnt);
        else
            SetFeedback(FB_INFO, "洲际导弹已入库：已为 %d 处选择器恢复原本攻击范围", sCnt);
        needRefreshSnap = false;
        break;
    }

    case REQ_TOGGLE_IGNORE_MOTION:
    {
        bool wantOn = (InterlockedCompareExchange(&g_ruleIgnoreMotion, 0, 0) == 0);
        InterlockedExchange(&g_ruleIgnoreMotion, wantOn ? 1 : 0);
        int sCnt = 0, revCnt = 0;
        ApplyRuleIgnoreMotion(wantOn, g_selectedCharId, &sCnt, &revCnt);
        if (wantOn)
            SetFeedback(FB_OK, "海陆空通吃照妖已开启！放开 %d 处地空索敌 + 照显 %d 只敌方单位（奶妈仍只奶队友）",
                        sCnt, revCnt);
        else
            SetFeedback(FB_INFO, "海陆空通吃已关闭：已为 %d 处选择器还原原始对地/对空规则", sCnt);
        needRefreshSnap = false;
        break;
    }

    case REQ_TOGGLE_IMMUNE:
    {
        bool wantOn = (InterlockedCompareExchange(&g_lockImmune, 0, 0) == 0);
        InterlockedExchange(&g_lockImmune, wantOn ? 1 : 0);
        if (wantOn)
        {
            int hit = ApplyCharImmune(g_selectedCharId, true, matched, sizeof(matched));
            g_applied.charAttrMask |= (1ULL << 26) | (1ULL << 31) | (1ULL << 32);
            SetFeedback(FB_OK, hit > 0
                        ? "泰拉霸体绝缘体已附体！%d 名干员免控+免元素损伤，晕眩/冰冻/神经损伤统统弹开"
                        : "泰拉霸体绝缘体已开启（已预设手牌，干员下场立刻百毒不侵！）", hit);
        }
        else
            SetFeedback(FB_INFO, "霸体绝缘体持续清控已关闭（已写入的本局高抵抗仍保留）");
        needRefreshSnap = false;
        break;
    }

    case REQ_MOD_CHAR_TRUE_DMG:
    {
        int sk = 0;
        int c1 = ApplyCharacterAttribute(g_req.text, 33 /* DAMAGE_HITRATE_PHYSICAL */, 1.0f, matched, sizeof(matched), nullptr, &sk);
        int c2 = ApplyCharacterAttribute(g_req.text, 34 /* DAMAGE_HITRATE_MAGICAL */,  1.0f, matched, sizeof(matched));
        int c3 = ApplyCharacterAttribute(g_req.text, 17 /* DEF_PENETRATE */,           1.0f, matched, sizeof(matched));
        int c4 = ApplyCharacterAttribute(g_req.text, 18 /* MAGIC_RESIST_PENETRATE */,  1.0f, matched, sizeof(matched));
        g_applied.trueDmg = true;
        g_applied.charAttrMask |= (1ULL << 17) | (1ULL << 18) | (1ULL << 33) | (1ULL << 34);
        int tot = c1 + c2 + c3 + c4;
        SetFeedback(FB_OK, tot > 0
                    ? "因果律必中真伤已觉醒！%s 100%% 命中 + 无视全部物防法抗（闪避/高甲通通蒸发）"
                    : "目标干员已是因果律真伤完全体（手牌亦同步附魔）",
                    matched[0] ? matched : g_selectedCharLabel);
        break;
    }

    case REQ_MOD_CHAR_MEGA_FORCE:
    {
        int sk = 0;
        int cnt = ApplyCharacterAttribute(g_req.text, 24 /* BASE_FORCE_LEVEL */, g_charForce, matched, sizeof(matched), nullptr, &sk);
        g_applied.megaForce = true;
        g_applied.charAttrMask |= (1ULL << 24);
        SetFeedback(FB_OK, cnt > 0
                    ? "大力出奇迹！！%d 名干员 (%s) 推拉力度拉满至 %.0f（搭配怪物零重量，一脚踹出银河系！）"
                    : "目标干员推拉力度已是 %.0f 级神力（手牌亦已注入神力）",
                    cnt > 0 ? cnt : sk, matched[0] ? matched : g_selectedCharLabel, g_charForce);
        break;
    }

    case REQ_MOD_CHAR_REFUND:
    {
        int cnt = ApplyCharFullRefund(g_req.text, matched, sizeof(matched));
        g_applied.fullRefund = true;
        SetFeedback(FB_OK, cnt > 0
                    ? "全额退款不粘锅生效！%d 名场上干员撤退 100%% 退全款（后续新下干员自动享七天无理由退款）"
                    : "撤退 100%% 返费已开启：后续上场的干员撤退一律原价全额退款！", cnt);
        break;
    }

    case REQ_MOD_ENEMY_ATTR:
    {
        float beforeVal = 0.0f;
        int skipped = 0;
        int cnt = ApplyEnemyAttribute(g_req.text, g_req.attrType, g_req.val, matched, sizeof(matched), &beforeVal, &skipped);
        const char *attrName =
            (g_req.attrType == 0)  ? "血量" :
            (g_req.attrType == 1)  ? "攻击" :
            (g_req.attrType == 2)  ? "防御" :
            (g_req.attrType == 3)  ? "法抗" :
            (g_req.attrType == 6)  ? "移速" :
            (g_req.attrType == 7)  ? "攻速" :
            (g_req.attrType == 23) ? "重量" : "属性";
        if (cnt > 0)
        {
            if (g_req.attrType >= 0 && g_req.attrType < 64)
                g_applied.enemyAttrMask |= (1ULL << g_req.attrType);
            if (g_req.attrType == 6) g_applied.enemyFrozen = (g_req.val <= 0.01f);
            SetFeedback(FB_OK, "敌方改造成功！%d 只倒霉蛋 (%s) %s：%.2f -> %.2f",
                        cnt, matched[0] ? matched : "全场敌人", attrName, beforeVal, g_req.val);
        }
        else if (skipped > 0)
        {
            if (g_req.attrType == 6) g_applied.enemyFrozen = (g_req.val <= 0.01f);
            SetFeedback(FB_INFO, "这 %d 只怪物的%s早就改成了 %.2f，别再补刀啦", skipped, attrName, g_req.val);
        }
        else
            SetFeedback(FB_WARN, "场上目前没看见 \"%s\"，等红门放怪再下手！", g_req.text[0] ? g_req.text : "敌人");
        break;
    }

    case REQ_MOD_ENEMY_SANDBAG:
    {
        int sk = 0;
        int cnt = ApplyEnemyAttribute(g_req.text, 0, 1000000.0f, matched, sizeof(matched), nullptr, &sk);
        ApplyEnemyAttribute(g_req.text, 1, 0.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 2, 0.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 3, 0.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 6, 0.0f, matched, sizeof(matched));
        ApplyEnemyUnharmful(g_req.text, matched, sizeof(matched));
        if (cnt > 0 || sk > 0)
        {
            g_applied.enemySandbag   = true;
            g_applied.enemyFrozen    = true;
            g_applied.enemyUnharmful = true;
            SetFeedback(cnt > 0 ? FB_OK : FB_INFO, cnt > 0
                        ? "沙包流水线出货！%d 只怪物已沦为 100 万血/0 攻防/罚站木桩（进蓝门不扣血）"
                        : "这 %d 只倒霉蛋早就是顶级沙包木桩了，尽情殴打吧！", cnt > 0 ? cnt : sk);
        }
        else
            SetFeedback(FB_WARN, "场上还没刷怪，沙包厂暂时停工待料");
        break;
    }

    case REQ_MOD_ENEMY_UNHARMFUL:
    {
        int sk = 0;
        int cnt = ApplyEnemyUnharmful(g_req.text, matched, sizeof(matched), &sk);
        if (cnt > 0)
        {
            g_applied.enemyUnharmful = true;
            SetFeedback(FB_OK, "发放蓝门观光门票！%d 只怪物现已文明旅游，进蓝门绝不扣血", cnt);
        }
        else if (sk > 0)
        {
            g_applied.enemyUnharmful = true;
            SetFeedback(FB_INFO, "场上 %d 只怪物早就领过文明游客证啦", sk);
        }
        else
            SetFeedback(FB_WARN, "场上暂无存活怪物");
        break;
    }

    case REQ_MOD_ENEMY_WEIGHTLESS:
    {
        int sk = 0;
        int cnt = ApplyEnemyAttribute(g_req.text, 23 /* MASS_LEVEL */, 0.0f, matched, sizeof(matched), nullptr, &sk);
        if (cnt > 0 || sk > 0)
        {
            g_applied.enemyWeightless = true;
            g_applied.enemyAttrMask |= (1ULL << 23);
            SetFeedback(cnt > 0 ? FB_OK : FB_INFO, cnt > 0
                        ? "牛顿棺材板按不住了！%d 只怪物已变为 0 重量氢气球（轻轻一推直飞平流层！）"
                        : "场上 %d 只怪物早就轻如鸿毛（重量=0），快请推拉干员起飞！", cnt > 0 ? cnt : sk);
        }
        else
            SetFeedback(FB_WARN, "场上暂无存活怪物可供抽干重力");
        break;
    }

    case REQ_MOD_ENEMY_REVEAL:
    {
        int cnt = ApplyEnemyRevealStealth(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "全图照妖镜发威！已剥离 %d 只怪物 (%s) 的隐匿与迷彩，无处遁形！",
                        cnt, matched[0] ? matched : "全场敌人");
        else
            SetFeedback(FB_WARN, "场上暂无存活怪物可照");
        break;
    }

    case REQ_MOD_ENEMY_KILL:
    {
        int cnt = ApplyEnemyKill(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "灭霸打了个响指！%d 只怪物 (%s) 在主线程安全灰飞烟灭", cnt, matched[0] ? matched : "全场敌人");
        else
            SetFeedback(FB_WARN, "响指落空：场上已经一只活怪都没啦");
        break;
    }

    case REQ_SCAN_STAGE_ENEMIES:
    {
        int cnt = ScanStageEnemies();
        if (cnt > 0)
            SetFeedback(FB_OK, "红门花名册已破译：本关共预加载 %d 种怪物（%ld 条行进路线）", cnt, (long)g_stageRouteCount);
        else
            SetFeedback(FB_WARN, "未读取到本关怪物名单");
        needRefreshSnap = false;
        break;
    }

    case REQ_SPAWN_ENEMY:
    {
        if (g_stageEnemyCount <= 0)
            ScanStageEnemies();
        const char *keyToUse = g_req.text;
        if ((!keyToUse || !keyToUse[0]) && g_stageEnemyCount > 0)
        {
            keyToUse = g_stageEnemies[0].id;
            CopyStrSafe(g_spawnEnemyKey, sizeof(g_spawnEnemyKey), keyToUse);
        }
        if (!keyToUse || !keyToUse[0])
        {
            SetFeedback(FB_WARN, "召唤失败：请先从花名册里点选一个倒霉蛋");
            break;
        }
        ExecuteSpawnEnemyOnMainThread(keyToUse, g_req.extraInt1, g_req.extraInt2, g_spawnApplyCustom);
        break;
    }

    default:
        needRefreshSnap = false;
        break;
    }

    if (needRefreshSnap && InterlockedCompareExchange(&g_battlePhase, 0, 0) == PHASE_PLAYING)
        RefreshBattleSnapshot();

    InterlockedIncrement(&g_reqDone);
}

// ================================================================ 扩展点实现

void Mod_OnReady()
{
    Log("[user] Mod_OnReady —— 初始化罗德岛物理法则改写终端（主线程安全模式）+ 博士实验室");

    ArkLogInit((HMODULE)ModSelfBase());
    ArkGuardInstall();
    ArkTaskBusInit();
    if (HWND hwnd = UiGameHwnd())
        ArkTaskBusSetMainThread(GetWindowThreadProcessId(hwnd, NULL));

    int clsCount = ArkReflectBuildIndex();
    Log("[user] 类索引构建完成: %d 个类", clsCount);
    ArkCfgLoad();

    Il2CppImage *img = ir::Image(MOD_MAIN_IMAGE);
    Log("[user] 主程序集镜像 %s = %p", MOD_MAIN_IMAGE, (void *)img);
}

// Worker 线程，每 100ms 一次 —— 严禁直接修改任何 Unity 战斗/UI 对象（只跑纯后台辅助任务）
void Mod_OnTick()
{
    if (InterlockedExchange(&g_dumpPending, 0) != 0)
    {
        Log("[user] dump 请求：%s", g_dumpText);
        ReflectDumpClass(g_dumpText, nullptr, 200);
        SetFeedback(FB_OK, "已将 \"%s\" 的灵魂结构导出至 il2cpp_mod.log", g_dumpText);
        InterlockedIncrement(&g_reqDone);
    }

    ArkTaskPump();
    ++g_tickCounter;

    // 每 500ms 刷新 Watch 缓存与配置落盘
    if ((g_tickCounter % 5) == 0)
    {
        ArkWatchRefresh();
        if (ArkCfgTakeDirty())
            ArkCfgSave();
    }
}

// 游戏渲染线程，每帧（面板关着也跑）：按需/100ms 投递 WM_APP+0x4D4F 驱动主线程消息泵
void Mod_OnFrame()
{
    HWND hwnd = UiGameHwnd();
    if (ArkTaskBusMainThread() == 0 && hwnd)
        ArkTaskBusSetMainThread(GetWindowThreadProcessId(hwnd, NULL));

    if (hwnd)
    {
        const ULONGLONG now = GetTickCount64();
        const bool hasUrgent = (InterlockedCompareExchange(&g_req.kind, 0, 0) != REQ_NONE) ||
                               (ArkMainTaskPending() > 0);
        if (hasUrgent || (now - g_lastPumpTick) >= 100ULL)
        {
            g_lastPumpTick = now;
            PostMessageW(hwnd, WM_APP + 0x4D4F, 0, 0);
        }
    }
    ArkHotkeyPoll();
}

// Unity 主线程（DetourClassFromName / ExitWndProc）
void Mod_OnGameThread()
{
    ArkWatchApplyWrites();
    if (ArkTaskBusIsMainThread())
    {
        ir::EnsureAttached();
        ArkMainTaskPump();
    }
}

// Unity 主线程顶层 Windows 消息泵边界（由 ExitWndProc 处理 WM_APP+0x4D4F 时独占调用）
// 绝不会嵌套在 CanvasUpdateRegistry.PerformUpdate 或 FixedUpdate 循环内部 -> 0 竞态、0 闪退！
void Mod_OnMainPump()
{
    if (InterlockedCompareExchange(&g_inMainPump, 1, 0) != 0)
        return;

    ir::EnsureAttached();

    // ① 原子守卫：先判断战斗阶段（m_state / m_result / m_haveGameLoaded），通关瞬间立刻停锁停写
    PollBattleSessionGuard();

    // ② 消费全部按钮修改请求（100% 主线程安全执行）
    ConsumeBattleRequestOnMainThread();

    const bool playingNow = (InterlockedCompareExchange(&g_battlePhase, 0, 0) == PHASE_PLAYING);
    ++g_mainTick;

    // ③ 持续自动锁定 & 规则增量同步（全幂等，仅对新上场干员/新刷出怪物增量生效，0 卡顿）
    if (playingNow)
    {
        if (InterlockedCompareExchange(&g_lockBattleDp, 0, 0) != 0)
            ApplyBattleDp(99);
        if ((g_mainTick % 2) == 0 &&
            (g_lockInfAmmo || g_lockInfSp || g_lockGodHp || g_lockImmune))
        {
            PumpPersistentLocks();
        }
        if ((g_mainTick % 4) == 0)
        {
            if (InterlockedCompareExchange(&g_ruleGlobalRange, 0, 0) != 0)
                ApplyRuleGlobalRange(true, g_selectedCharId, nullptr, nullptr);
            if (InterlockedCompareExchange(&g_ruleIgnoreMotion, 0, 0) != 0)
                ApplyRuleIgnoreMotion(true, g_selectedCharId, nullptr, nullptr);
            if (g_applied.fullRefund)
            {
                char dummy[64];
                ApplyCharFullRefund(g_selectedCharId, dummy, sizeof(dummy));
            }
        }
    }

    // ④ 面板展开时每 1s 零 GC 刷新一次实时编队/怪物快照（<0.05ms）
    static bool s_prevPanelVis = false;
    const bool panelVis = UiPanelVisible();
    if (panelVis && playingNow && (!s_prevPanelVis || (g_mainTick % 10) == 0))
    {
        RefreshBattleSnapshot();
    }
    s_prevPanelVis = panelVis;

    InterlockedExchange(&g_inMainPump, 0);
}

static ImVec4 FeedbackColor(int level)
{
    switch (level)
    {
    case FB_OK:   return ImVec4(0.35f, 1.00f, 0.50f, 1.0f);
    case FB_WARN: return ImVec4(1.00f, 0.82f, 0.30f, 1.0f);
    case FB_ERR:  return ImVec4(1.00f, 0.40f, 0.40f, 1.0f);
    default:      return ImVec4(0.75f, 0.90f, 1.00f, 1.0f);
    }
}

static const char *FeedbackPrefix(int level)
{
    switch (level)
    {
    case FB_OK:   return "[✓ 成功]";
    case FB_WARN: return "[! 提示]";
    case FB_ERR:  return "[✗ 失败]";
    default:      return "[i 状态]";
    }
}

static void ItemTip(const char *tip)
{
    if (tip && tip[0] && ImGui::IsItemHovered())
    {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(380.0f);
        ImGui::TextUnformatted(tip);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

// ---- 状态高亮按钮辅助（已开启/已生效自动变色，一眼看清当前状态）----
static bool DrawToggleStateBtn(const char *labelOn, const char *labelOff, bool active, float w = 0.0f, float h = 28.0f)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.14f, 0.56f, 0.30f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.18f, 0.66f, 0.36f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.10f, 0.45f, 0.24f, 1.0f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.22f, 0.25f, 0.31f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.29f, 0.33f, 0.41f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.21f, 0.26f, 1.0f));
    }
    bool clicked = ImGui::Button(active ? labelOn : labelOff, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    return clicked;
}

// 规则粉碎机专属亮紫/靛蓝高亮按钮（区分「底层规则改写」与普通数值锁定）
static bool DrawRuleToggleBtn(const char *labelOn, const char *labelOff, bool active, float w = 0.0f, float h = 28.0f)
{
    if (active)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.50f, 0.20f, 0.68f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.60f, 0.26f, 0.80f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.40f, 0.15f, 0.56f, 1.0f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.25f, 0.23f, 0.35f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.34f, 0.30f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.18f, 0.29f, 1.0f));
    }
    bool clicked = ImGui::Button(active ? labelOn : labelOff, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    return clicked;
}

static bool DrawAppliedActionBtn(const char *labelDone, const char *labelIdle, bool applied,
                                 bool goldAccent = false, float w = 0.0f, float h = 28.0f)
{
    if (applied)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        goldAccent ? ImVec4(0.68f, 0.48f, 0.10f, 1.0f) : ImVec4(0.14f, 0.50f, 0.34f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, goldAccent ? ImVec4(0.78f, 0.56f, 0.14f, 1.0f) : ImVec4(0.18f, 0.60f, 0.40f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  goldAccent ? ImVec4(0.55f, 0.38f, 0.08f, 1.0f) : ImVec4(0.10f, 0.40f, 0.27f, 1.0f));
    }
    else if (goldAccent)
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.24f, 0.36f, 0.54f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.45f, 0.66f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.20f, 0.30f, 0.46f, 1.0f));
    }
    else
    {
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.23f, 0.28f, 0.36f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.30f, 0.36f, 0.46f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.18f, 0.23f, 0.30f, 1.0f));
    }
    bool clicked = ImGui::Button(applied ? labelDone : labelIdle, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    return clicked;
}

static bool DrawDangerBtn(const char *label, float w = 0.0f, float h = 26.0f)
{
    ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0.62f, 0.18f, 0.18f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.76f, 0.24f, 0.24f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(0.50f, 0.14f, 0.14f, 1.0f));
    bool clicked = ImGui::Button(label, ImVec2(w, h));
    ImGui::PopStyleColor(3);
    return clicked;
}

static void DrawBattleCustomizerTab()
{
    // 拷贝当前快照与本局已生效状态（零阻塞、线程安全）
    StageCharInfo localChars[32];
    int localCharCount = 0;
    LiveEnemyGroup localEnemies[32];
    int localEnemyCount = 0;
    int localEnemyTotal = 0;
    BattleAppliedState applied;

    SpinLock(&g_snapLock);
    localCharCount = g_stageCharCount;
    for (int i = 0; i < localCharCount; ++i) localChars[i] = g_stageChars[i];
    localEnemyCount = g_liveEnemyCount;
    localEnemyTotal = g_liveEnemyTotal;
    for (int i = 0; i < localEnemyCount; ++i) localEnemies[i] = g_liveEnemies[i];
    applied = g_applied;
    SpinUnlock(&g_snapLock);

    FeedbackEntry curFb;
    FeedbackEntry histCopy[8];
    int histCount = 0;
    SpinLock(&g_fbLock);
    curFb = g_lastFeedback;
    histCount = g_fbCount;
    for (int i = 0; i < histCount; ++i)
    {
        int idx = (g_fbHead - 1 - i + 8) & 7;
        histCopy[i] = g_fbHist[idx];
    }
    SpinUnlock(&g_fbLock);

    const BattlePhase curPhase = (BattlePhase)InterlockedCompareExchange(&g_battlePhase, 0, 0);
    const bool isAutoReplay    = (InterlockedCompareExchange(&g_isAutoReplay, 0, 0) != 0);
    const LONG curSessId       = InterlockedCompareExchange(&g_battleSessId, 0, 0);
    const bool battleReady     = (curPhase == PHASE_PLAYING);

    // ========================================================================
    // 1. 顶部紧凑状态栏（单行整合：阶段徽章 + 刷新/复位/防脏开关）
    // ========================================================================
    {
        ImVec4 stCol;
        char stText[200];
        if (curPhase == PHASE_PLAYING && isAutoReplay)
        {
            stCol = ImVec4(1.00f, 0.60f, 0.20f, 1.0f);
            _snprintf_s(stText, sizeof(stText), _TRUNCATE, "⚠ 代理指挥录像 #%ld · %s（持续锁已自动休眠，防把自律改劈叉）",
                        (long)curSessId, g_curLevelId[0] ? g_curLevelId : "战斗中");
        }
        else if (curPhase == PHASE_PLAYING)
        {
            stCol = ImVec4(0.32f, 0.96f, 0.56f, 1.0f);
            _snprintf_s(stText, sizeof(stText), _TRUNCATE, "● 战场已接管 #%ld · %s（主线程稳如老狗 · 零卡顿）",
                        (long)curSessId, g_curLevelId[0] ? g_curLevelId : "关卡");
        }
        else if (curPhase == PHASE_LOADING)
        {
            stCol = ImVec4(1.00f, 0.85f, 0.35f, 1.0f);
            CopyStrSafe(stText, sizeof(stText), "○ 干员空投下坠中... 落地进场后自动解锁黑科技");
        }
        else if (curPhase == PHASE_FINISHED)
        {
            stCol = ImVec4(0.95f, 0.72f, 0.35f, 1.0f);
            CopyStrSafe(stText, sizeof(stText), "○ 战役大捷结算中 · 改写器已安全锁仓（零闪退护体）");
        }
        else
        {
            stCol = ImVec4(0.58f, 0.82f, 1.00f, 1.0f);
            CopyStrSafe(stText, sizeof(stText), "○ 基地大厅摸鱼中 · 随便进个关卡立刻接管物理法则");
        }

        ImGui::TextColored(stCol, "%s", stText);
        ImGui::SameLine();
        if (ImGui::SmallButton("雷达刷新"))
            PostRequestEx(REQ_SCAN_ROSTER, "", 0, 0.0f);
        ItemTip("立刻重扫本局编队干员与场上怪物快照");
        ImGui::SameLine();
        if (ImGui::SmallButton("一键归零(复位)"))
            PostRequestEx(REQ_RESET_SESSION, "", 0, 0.0f);
        ItemTip("关闭全部锁定 + 还原全地形/叠放/全图射程/地空规则 + 恢复 1x 速度 + 切回全体目标");
        ImGui::SameLine();
        bool autoRst = (InterlockedCompareExchange(&g_autoResetNew, 0, 0) != 0);
        if (ImGui::Checkbox("切关自动撤电(清锁)", &autoRst))
        {
            InterlockedExchange(&g_autoResetNew, autoRst ? 1 : 0);
            SetFeedback(FB_INFO, autoRst
                        ? "已开启切关/结算自动断电清锁（杜绝把脏状态带进下一关）"
                        : "已关闭切关自动清锁（通关结算保护与代理指挥保护仍坚守岗位）");
        }
        ItemTip("切关、重开或结算时自动关闭所有持续锁定并清理上一局状态（强烈建议保持勾选）");
    }

    // 单行紧凑实时反馈条
    {
        ImVec4 col = FeedbackColor(curFb.level);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(col.x * 0.13f, col.y * 0.13f, col.z * 0.13f, 0.85f));
        ImGui::PushStyleColor(ImGuiCol_Border,  ImVec4(col.x * 0.75f, col.y * 0.75f, col.z * 0.75f, 0.80f));
        if (ImGui::BeginChild("##FbBar", ImVec2(0.0f, 30.0f), true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::TextColored(col, "%s %s  %s",
                               FeedbackPrefix(curFb.level),
                               curFb.timeStr[0] ? curFb.timeStr : "--:--:--",
                               curFb.msg);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    ImGui::BeginDisabled(!battleReady);

    // ========================================================================
    // 2. 物理法则总控台（4×3 对齐状态矩阵 + 时间法则 + 一键地爆天星超载）
    //    翠绿 = 锁定/生效中 | 亮紫 = 底层规则粉碎机激活中 | 金色 = 终极超载已激活
    // ========================================================================
    if (ImGui::BeginTable("##QuickDeck", 4, ImGuiTableFlags_SizingStretchSame))
    {
        // Row 1: 4 项持续自动锁定（绿色常亮 = 开启中）
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        {
            bool on = (InterlockedCompareExchange(&g_lockInfAmmo, 0, 0) != 0);
            if (DrawToggleStateBtn("● 弹幕永动机 (无限弹药)###lk_ammo", "○ 弹幕永动机 (无限弹药)###lk_ammo", on, -1.0f, 28.0f))
            {
                InterlockedExchange(&g_lockInfAmmo, on ? 0 : 1);
                SetFeedback(FB_OK, "「弹幕永动机 (无限弹药锁定)」已%s（目标：%s）", on ? "关闭" : "点火", g_selectedCharLabel);
            }
            ItemTip("持续锁定弹药类干员（如能天使/假日威龙陈/水陈等）的子弹不消耗，射到地老天荒");
        }
        ImGui::TableSetColumnIndex(1);
        {
            bool on = (InterlockedCompareExchange(&g_lockInfSp, 0, 0) != 0);
            if (DrawToggleStateBtn("● 永动充电宝 (无限技力)###lk_sp", "○ 永动充电宝 (无限技力)###lk_sp", on, -1.0f, 28.0f))
            {
                InterlockedExchange(&g_lockInfSp, on ? 0 : 1);
                SetFeedback(FB_OK, "「永动充电宝 (无限技力锁定)」已%s（目标：%s）", on ? "拔线" : "满充", g_selectedCharLabel);
            }
            ItemTip("持续把目标干员的技力条顶满，技能随便连发无需等回转");
        }
        ImGui::TableSetColumnIndex(2);
        {
            bool on = (InterlockedCompareExchange(&g_lockGodHp, 0, 0) != 0);
            if (DrawToggleStateBtn("● 金身不坏老赖 (锁血无敌)###lk_hp", "○ 金身不坏老赖 (锁血无敌)###lk_hp", on, -1.0f, 28.0f))
            {
                InterlockedExchange(&g_lockGodHp, on ? 0 : 1);
                SetFeedback(FB_OK, "「金身不坏老赖 (锁血无敌)」已%s（目标：%s）", on ? "卸甲" : "开盾", g_selectedCharLabel);
            }
            ItemTip("持续把干员生命值锁死在 100%，站撸 Boss 纹丝不动（不卡血条 UI）");
        }
        ImGui::TableSetColumnIndex(3);
        {
            bool on = (InterlockedCompareExchange(&g_lockBattleDp, 0, 0) != 0);
            if (DrawToggleStateBtn("● 印钞机狂转 (DP锁99)###lk_dp", "○ 印钞机狂转 (DP锁99)###lk_dp", on, -1.0f, 28.0f))
            {
                InterlockedExchange(&g_lockBattleDp, on ? 0 : 1);
                SetFeedback(FB_OK, "「罗德岛印钞机 (DP 锁 99)」已%s", on ? "停机" : "狂印");
            }
            ItemTip("持续把战场部署费用 DP 维持在 99 点（上限提至 999），先锋干员当场失业");
        }

        // Row 2: 4 项战场一键强化（生效即变绿 [✓]，防重复叠加）
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        if (DrawAppliedActionBtn("✓ 白嫖天降 (手牌0费)###act_cost", "白嫖天降 (手牌0费)###act_cost", applied.zeroCost, false, -1.0f, 28.0f))
            PostRequestEx(REQ_MOD_CARD_COST, g_selectedCharId, 4, (float)g_cardCostDelta);
        ItemTip("把选定干员（默认全体）的手牌部署费用直接砍到 0 费，开局全员空降");

        ImGui::TableSetColumnIndex(1);
        if (DrawAppliedActionBtn("✓ 诈尸返场 (0秒重下)###act_resp", "诈尸返场 (0秒重下)###act_resp", applied.zeroRespawn, false, -1.0f, 28.0f))
            PostRequestEx(REQ_MOD_CARD_RESPAWN, g_selectedCharId, 21, 1.0f);
        ItemTip("把再部署时间缩到 1 秒并立刻清空当前复活倒计时，刚撤退就能秒下");

        ImGui::TableSetColumnIndex(2);
        if (DrawAppliedActionBtn("✓ 人口大爆炸 (上限+99)###act_lim", "人口大爆炸 (上限+99)###act_lim", applied.deployLimit, false, -1.0f, 28.0f))
            PostRequestEx(REQ_MOD_DEPLOY_LIMIT, g_selectedCharId, 0, 0.0f);
        ItemTip("解除本局部署人数限制（手牌不占部署位 + 战场人口上限一次性 +99，防重复叠加）");

        ImGui::TableSetColumnIndex(3);
        if (DrawAppliedActionBtn("✓ 焊死蓝门 (生命999)###act_lp", "焊死蓝门 (生命999)###act_lp", applied.life999, false, -1.0f, 28.0f))
            PostRequestEx(REQ_MOD_BATTLE_LIFE, "", 0, 999.0f);
        ItemTip("把我方目标生命点（蓝门血量）直接焊到 999，漏怪漏到手软也不掉星");

        // Row 3: 4 项底层规则粉碎机（亮紫常亮 = 已改写底层物理法则，可随时再点一键还原！）
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        {
            bool on = (InterlockedCompareExchange(&g_ruleAllTerrain, 0, 0) != 0);
            if (DrawRuleToggleBtn("★ 违章乱建 (全地形放置)###rl_ter", "○ 违章乱建 (全地形放置)###rl_ter", on, -1.0f, 28.0f))
                PostRequestEx(REQ_TOGGLE_ALL_TERRAIN, "", 0, 0.0f);
            ItemTip("调用原生 Tile.OverwriteBuildableType/AdvancedBuildableMask 放开全图地块与特殊地形(深海/红雾/禁区)放置限制：近战上高台、狙击站地板随便摆（自动保护红蓝门/传送门，再点一键还原）");
        }
        ImGui::TableSetColumnIndex(1);
        {
            bool on = (InterlockedCompareExchange(&g_ruleOverlap, 0, 0) != 0);
            if (DrawRuleToggleBtn("★ 叠罗汉 (同格无限叠放)###rl_ovl", "○ 叠罗汉 (同格无限叠放)###rl_ovl", on, -1.0f, 28.0f))
                PostRequestEx(REQ_TOGGLE_OVERLAP, "", 0, 0.0f);
            ItemTip("允许在同一个格子上叠加部署多名干员，把十个大爹塞进一个格子里（再点一键还原）");
        }
        ImGui::TableSetColumnIndex(2);
        {
            bool on = (InterlockedCompareExchange(&g_ruleGlobalRange, 0, 0) != 0);
            if (DrawRuleToggleBtn("★ 洲际导弹 (全图射程)###rl_rng", "○ 洲际导弹 (全图射程)###rl_rng", on, -1.0f, 28.0f))
                PostRequestEx(REQ_TOGGLE_GLOBAL_RANGE, g_selectedCharId, 0, 0.0f);
            ItemTip("远程与近战(BlockedOrAdvancedSelector)及复合技能通通挂载游戏原生 GlobalRange 全图射程：有怪贴脸优先砍阻挡，没怪贴脸全图隔山打牛（新下干员与开技能自动继承，再点还原）");
        }
        ImGui::TableSetColumnIndex(3);
        {
            bool on = (InterlockedCompareExchange(&g_ruleIgnoreMotion, 0, 0) != 0);
            if (DrawRuleToggleBtn("★ 海陆空通吃 (地空+破隐)###rl_mot", "○ 海陆空通吃 (地空+破隐)###rl_mot", on, -1.0f, 28.0f))
                PostRequestEx(REQ_TOGGLE_IGNORE_MOTION, g_selectedCharId, 0, 0.0f);
            ItemTip("无视对地/对空目标限制（地面近卫手撕无人机）+ 自动破除敌方隐匿与迷彩（绝不串台：奶妈只奶队友、输出只打敌人）");
        }

        ImGui::EndTable();
    }

    // Row 4: 时间法则（单选变色高亮）+ 地爆天星一键超载
    {
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("时间法则:");
        ImGui::SameLine();
        const char *spdLabels[4] = { "子弹0.5x###sp0", "凡人1x###sp1", "快进2x###sp2", "光速3x###sp3" };
        const float spdVals[4]   = { 0.5f, 1.0f, 2.0f, 3.0f };
        const char *spdTips[4]   = {
            "子弹时间 (0.5x 慢速)：极限微操卡轴专用",
            "凡人流速 (1x 标准速度)：原汁原味",
            "二倍快进 (2x 原生加速)：日常刷图标准节奏",
            "光速起飞 (3x 原生 SUPER_FAST 极速档)"
        };
        for (int s = 0; s < 4; ++s)
        {
            if (s > 0) ImGui::SameLine();
            bool sel = (applied.speedMode == 0 && applied.speedLevel == s);
            if (DrawToggleStateBtn(spdLabels[s], spdLabels[s], sel, 72.0f, 26.0f))
                PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, spdVals[s], 0, s);
            ItemTip(spdTips[s]);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(86.0f);
        ImGui::SliderFloat("##customTs", &g_customTimeScale, 0.25f, 6.0f, "%.2fx");
        ImGui::SameLine();
        if (DrawToggleStateBtn("✓ 扭曲###spC", "扭曲(应用)###spC", applied.speedMode == 1, 78.0f, 26.0f))
            PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, g_customTimeScale, 1, 0);
        ItemTip("按左侧滑条自定义任意战场时间倍率 (0.25x ~ 6.0x)");

        ImGui::SameLine();
        if (DrawAppliedActionBtn("★ 地爆天星超载 [已点火·点击补刷]###act_ov",
                                 "★ 地爆天星超载 (0费/高攻速/满弹药)###act_ov",
                                 applied.overload, true, -1.0f, 26.0f))
            PostRequestEx(REQ_MOD_ANGEL_PRESET, g_selectedCharId, 0, 0.0f);
        ItemTip("一键连招：0费 + 0秒重下 + 无限人口 + DP99 + 狂飙攻速/低间隔/高攻血/高回转 + 扩满弹匣");
    }

    ImGui::Spacing();

    // ========================================================================
    // 3. 按阵营分 2 个精简子页（我方神兵工坊 | 敌方怪物改造厂）
    // ========================================================================
    if (ImGui::BeginTabBar("##BattleSubTabs"))
    {
        // ------------------------------------------------------------
        // 子页 ①：罗德岛神兵工坊（我方干员 & 弹药 & 特殊外挂）
        // ------------------------------------------------------------
        if (ImGui::BeginTabItem("① 罗德岛神兵工坊 (我方干员)"))
        {
            // A. 目标干员选择（芯片 + 搜索合并为紧凑一行）
            ImGui::SetNextItemWidth(210.0f);
            bool searchEnter = ImGui::InputTextWithHint("##charSearch", "按中文名/ID 筛本局干员",
                                                        g_charSearch, sizeof(g_charSearch),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (g_charSearch[0])
            {
                if (ImGui::SmallButton("清空")) g_charSearch[0] = 0;
                ImGui::SameLine();
            }
            ImGui::TextColored(ImVec4(1.0f, 0.90f, 0.45f, 1.0f), "当前附魔目标: %s", g_selectedCharLabel);
            if (g_selectedCharId[0])
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("切回全体干员"))
                {
                    g_selectedCharId[0] = 0;
                    CopyStrSafe(g_selectedCharLabel, sizeof(g_selectedCharLabel), "全体罗德岛干员");
                }
            }

            // 本局编队芯片列表
            {
                bool allSel = (g_selectedCharId[0] == 0);
                if (allSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.56f, 0.34f, 1.0f));
                char allBtn[64];
                _snprintf_s(allBtn, sizeof(allBtn), _TRUNCATE, "★ 全军出击 (%d人)###chr_all", localCharCount);
                if (ImGui::SmallButton(allBtn))
                {
                    g_selectedCharId[0] = 0;
                    CopyStrSafe(g_selectedCharLabel, sizeof(g_selectedCharLabel), "全体罗德岛干员");
                }
                if (allSel) ImGui::PopStyleColor();

                int shown = 0;
                int firstMatchIdx = -1;
                for (int i = 0; i < localCharCount; ++i)
                {
                    const StageCharInfo &c = localChars[i];
                    if (g_charSearch[0] &&
                        !MatchSubstringIgnoreCase(c.nameCn, g_charSearch) &&
                        !MatchSubstringIgnoreCase(c.id, g_charSearch))
                        continue;
                    if (firstMatchIdx < 0) firstMatchIdx = i;

                    if ((shown + 1) % 5 != 0) ImGui::SameLine();
                    ++shown;

                    bool isSel = (g_selectedCharId[0] && strcmp(g_selectedCharId, c.id) == 0);
                    if (isSel)           ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.52f, 0.12f, 1.0f));
                    else if (c.deployed) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.56f, 1.0f));
                    else                 ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.26f, 0.32f, 1.0f));

                    char chip[160];
                    if (c.deployed && c.hasAmmo)
                        _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s [场上 弹%d/%d]###c%d",
                                    c.nameCn[0] ? c.nameCn : c.id, c.ammoRem, c.ammoMax, i);
                    else if (c.deployed)
                        _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s [场上]###c%d",
                                    c.nameCn[0] ? c.nameCn : c.id, i);
                    else
                        _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s [%d费]###c%d",
                                    c.nameCn[0] ? c.nameCn : c.id, c.cost, i);

                    if (ImGui::SmallButton(chip))
                    {
                        CopyStrSafe(g_selectedCharId, sizeof(g_selectedCharId), c.id);
                        _snprintf_s(g_selectedCharLabel, sizeof(g_selectedCharLabel), _TRUNCATE,
                                    "%s (%s)", c.nameCn[0] ? c.nameCn : c.id, c.id);
                    }
                    ImGui::PopStyleColor();
                }

                if (searchEnter && firstMatchIdx >= 0)
                {
                    const StageCharInfo &m = localChars[firstMatchIdx];
                    CopyStrSafe(g_selectedCharId, sizeof(g_selectedCharId), m.id);
                    _snprintf_s(g_selectedCharLabel, sizeof(g_selectedCharLabel), _TRUNCATE,
                                "%s (%s)", m.nameCn[0] ? m.nameCn : m.id, m.id);
                    SetFeedback(FB_OK, "附魔准星已锁定干员：%s", g_selectedCharLabel);
                }
                if (g_charSearch[0] && shown == 0 && localCharCount > 0)
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f), "本局编队里查无此人：\"%s\"", g_charSearch);
            }

            // 选中干员实时数值回显条
            if (g_selectedCharId[0])
            {
                const StageCharInfo *sel = nullptr;
                for (int i = 0; i < localCharCount; ++i)
                    if (strcmp(localChars[i].id, g_selectedCharId) == 0) { sel = &localChars[i]; break; }
                if (sel)
                {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.14f, 0.19f, 0.90f));
                    if (ImGui::BeginChild("##CharStat", ImVec2(0.0f, 46.0f), true, ImGuiWindowFlags_NoScrollbar))
                    {
                        ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.70f, 1.0f),
                                           "%s [%s]  费:%d  血:%.0f/%.0f  攻:%.0f  攻速:%.0f  间隔:%.2fs",
                                           sel->nameCn[0] ? sel->nameCn : sel->id,
                                           sel->deployed ? "场上激战" : "手牌待命",
                                           sel->cost, sel->hp, sel->maxHp, sel->atk, sel->atkSpd, sel->baseAtkTime);
                        if (sel->hasAmmo)
                            ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.35f, 1.0f),
                                               "防:%.0f  法抗:%.0f  阻挡:%.0f  技力:%.1f/%.0f  ★弹匣: %d/%d",
                                               sel->def, sel->magicRes, sel->blockCnt, sel->sp, sel->maxSp,
                                               sel->ammoRem, sel->ammoMax);
                        else
                            ImGui::TextDisabled("防:%.0f  法抗:%.0f  阻挡:%.0f  技力:%.1f/%.0f  弹匣: 当前无激活弹药条",
                                                sel->def, sel->magicRes, sel->blockCnt, sel->sp, sel->maxSp);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
            }

            ImGui::Separator();

            // B. 4 项干员逆天词条（4列对齐，独立高亮状态）
            if (ImGui::BeginTable("##CharGodPowers", 4, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (DrawAppliedActionBtn("✓ 因果律真伤 (必中+双穿)###cg_td", "因果律真伤 (必中+双穿)###cg_td",
                                         applied.trueDmg, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_CHAR_TRUE_DMG, g_selectedCharId, 0, 0.0f);
                ItemTip("物理/法术 100% 必中 + 100% 无视防御与法抗，闪避怪与高甲罐头全部当场蒸发");

                ImGui::TableSetColumnIndex(1);
                {
                    bool on = (InterlockedCompareExchange(&g_lockImmune, 0, 0) != 0);
                    if (DrawToggleStateBtn("● 霸体绝缘体 (免控+免元素)###cg_im", "○ 霸体绝缘体 (免控+免元素)###cg_im",
                                           on, -1.0f, 26.0f))
                        PostRequestEx(REQ_TOGGLE_IMMUNE, g_selectedCharId, 0, 0.0f);
                    ItemTip("抵抗拉满 + 100% 免元素损伤 + 持续净化眩晕/冰冻/寒冷/沉默/麻痹/恐惧/沉睡");
                }

                ImGui::TableSetColumnIndex(2);
                if (DrawAppliedActionBtn("✓ 大力出奇迹 (推拉+10)###cg_mf", "大力出奇迹 (推拉+10)###cg_mf",
                                         applied.megaForce, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_CHAR_MEGA_FORCE, g_selectedCharId, 24, g_charForce);
                ItemTip("干员基础推拉力度直冲 +10（搭配敌方页「失重气球」，阿消一水枪能把大爹呲出地图）");

                ImGui::TableSetColumnIndex(3);
                if (DrawAppliedActionBtn("✓ 全额退款 (撤退100%返费)###cg_rf", "全额退款 (撤退100%返费)###cg_rf",
                                         applied.fullRefund, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_CHAR_REFUND, g_selectedCharId, 0, 1.0f);
                ItemTip("干员撤退时 100% 退还全部部署费用，真正实现七天无理由白嫖（后续新下干员自动继承）");

                ImGui::EndTable();
            }

            // C. 瞬间回满 + 弹药直改 + 紧急印钞（紧凑一行）
            if (ImGui::Button("原地满血满蓝 (回血+满SP)", ImVec2(188.0f, 26.0f)))
                PostRequestEx(REQ_MOD_CHAR_FULL_HPSP, g_selectedCharId, 0, 0.0f);
            ItemTip("立刻把场上选定干员的血条和技力条全部奶满");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(90.0f);
            ImGui::InputInt("##ammoVal", &g_charAmmoTarget, 10, 50);
            if (g_charAmmoTarget < 1) g_charAmmoTarget = 1;
            if (g_charAmmoTarget > 9999) g_charAmmoTarget = 9999;
            ImGui::SameLine();
            if (DrawAppliedActionBtn("✓ 魔改弹匣 (改弹药+上限)###am1", "魔改弹匣 (改弹药+上限)###am1", applied.ammoBoosted, false, 182.0f, 26.0f))
                PostRequestEx(REQ_MOD_CHAR_AMMO, g_selectedCharId, 0, (float)g_charAmmoTarget, 1, 0);
            ItemTip("同时扩容当前激活弹药技的弹匣上限与剩余子弹数");
            ImGui::SameLine();
            if (ImGui::Button("装弹补满 (仅补弹)", ImVec2(138.0f, 26.0f)))
                PostRequestEx(REQ_MOD_CHAR_AMMO, g_selectedCharId, 0, (float)g_charAmmoTarget, 0, 0);
            ImGui::SameLine();
            if (ImGui::Button("紧急印钞 (DP=99)", ImVec2(136.0f, 26.0f)))
                PostRequestEx(REQ_MOD_BATTLE_DP, "", 0, 99.0f);

            ImGui::Separator();

            // D. 干员实时属性精调表（2列×5行整齐对齐，幽默主标题 + 括号正经说明，已修改项按钮变绿 [✓]）
            if (ImGui::BeginTable("##CharAttrGrid", 2, ImGuiTableFlags_SizingStretchSame))
            {
                auto drawCharAttrCell = [&](const char *title, const char *idTag, float *val,
                                            float step, float stepFast, const char *fmt, int attrType, const char *tip) {
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputFloat(idTag, val, step, stepFast, fmt);
                    ImGui::SameLine();
                    bool done = (attrType >= 0 && attrType < 64) && ((applied.charAttrMask & (1ULL << attrType)) != 0);
                    char bDone[96], bIdle[96];
                    _snprintf_s(bDone, sizeof(bDone), _TRUNCATE, "✓ %s###ca_%d", title, attrType);
                    _snprintf_s(bIdle, sizeof(bIdle), _TRUNCATE, "%s###ca_%d", title, attrType);
                    if (DrawAppliedActionBtn(bDone, bIdle, done, false, -1.0f, 24.0f))
                        PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, attrType, *val);
                    ItemTip(tip);
                };

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawCharAttrCell("鬼畜攻速 (改攻速)",     "##c_aspd", &g_charAtkSpeed,    50.0f,   100.0f,  "%.0f", 7,  "修改干员攻击速度（默认100，设为500直接变加特林）");
                ImGui::TableSetColumnIndex(1); drawCharAttrCell("零帧起手 (改攻击间隔s)","##c_bat",  &g_charBaseAtkTime, 0.05f,   0.1f,    "%.2f", 8,  "修改基础攻击间隔（秒，越小出手越快，推荐 0.10~0.20）");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawCharAttrCell("核弹伤害 (改攻击力)",   "##c_atk",  &g_charAtk,         500.0f,  1000.0f, "%.0f", 1,  "修改干员攻击力（一刀一个小朋友）");
                ImGui::TableSetColumnIndex(1); drawCharAttrCell("肉山血牛 (改生命上限)", "##c_hp",   &g_charMaxHp,       5000.0f, 10000.0f,"%.0f", 0,  "修改干员最大生命值并立刻回满当前血量");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawCharAttrCell("移动要塞 (改防御力)",   "##c_def",  &g_charDef,         200.0f,  500.0f,  "%.0f", 2,  "修改干员物理防御力，物理怪打上来全是抛光");
                ImGui::TableSetColumnIndex(1); drawCharAttrCell("魔免圣体 (改法术抗性)", "##c_res",  &g_charMagicRes,    10.0f,   20.0f,   "%.0f", 3,  "修改干员法术抗性（95~100 近乎免疫法术伤害）");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawCharAttrCell("一夫当关 (改阻挡数)",   "##c_blk",  &g_charBlockCnt,    1.0f,    2.0f,    "%.0f", 5,  "修改干员最大阻挡数，高台狙击都能当盾用");
                ImGui::TableSetColumnIndex(1); drawCharAttrCell("核能回转 (改技力/秒)",  "##c_sp",   &g_charSpRecov,     5.0f,    10.0f,   "%.1f", 14, "修改每秒自动技力回复速度并立刻回满 SP");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawCharAttrCell("绝世显眼包 (改嘲讽值)", "##c_tnt",  &g_charTaunt,       1.0f,    5.0f,    "%.0f", 20, "正数(+10)疯狂吸引全图仇恨当沙包；负数(-10)彻底隐身不挨打");
                ImGui::TableSetColumnIndex(1); drawCharAttrCell("大力出奇迹 (改推拉力)", "##c_frc",  &g_charForce,       1.0f,    2.0f,    "%.0f", 24, "修改干员基础推拉力度 (BASE_FORCE_LEVEL)，数值越大推得越远");

                ImGui::EndTable();
            }

            ImGui::EndTabItem();
        }

        // ------------------------------------------------------------
        // 子页 ②：整合运动改造厂（敌方怪物改造 & 动态刷怪器）
        // ------------------------------------------------------------
        if (ImGui::BeginTabItem("② 整合运动改造厂 (敌方怪物 & 刷怪)"))
        {
            // A. 场上存活怪物选择
            {
                bool allE = (g_enemyFilter[0] == 0);
                if (allE) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.28f, 0.18f, 1.0f));
                char allBtn[64];
                _snprintf_s(allBtn, sizeof(allBtn), _TRUNCATE, "★ 全场倒霉蛋 (%d只)###enm_all", localEnemyTotal);
                if (ImGui::SmallButton(allBtn))
                {
                    g_enemyFilter[0] = 0;
                    CopyStrSafe(g_enemyLabel, sizeof(g_enemyLabel), "全场所有倒霉蛋");
                }
                if (allE) ImGui::PopStyleColor();

                for (int i = 0; i < localEnemyCount; ++i)
                {
                    const LiveEnemyGroup &e = localEnemies[i];
                    if ((i + 1) % 4 != 0) ImGui::SameLine();
                    bool isSel = (g_enemyFilter[0] && strcmp(g_enemyFilter, e.key) == 0);
                    if (isSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.48f, 0.12f, 1.0f));
                    char chip[160];
                    _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s ×%d###le%d",
                                e.name[0] ? e.name : e.key, e.count, i);
                    if (ImGui::SmallButton(chip))
                    {
                        CopyStrSafe(g_enemyFilter, sizeof(g_enemyFilter), e.key);
                        _snprintf_s(g_enemyLabel, sizeof(g_enemyLabel), _TRUNCATE,
                                    "%s (%s)", e.name[0] ? e.name : e.key, e.key);
                    }
                    if (isSel) ImGui::PopStyleColor();
                }
            }

            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::InputTextWithHint("##eflt", "怪物筛选 (中文/ID，留空=全场)", g_enemyFilter, sizeof(g_enemyFilter)))
                CopyStrSafe(g_enemyLabel, sizeof(g_enemyLabel), g_enemyFilter[0] ? g_enemyFilter : "全场所有倒霉蛋");
            ImGui::SameLine();
            ImGui::TextDisabled("当前改造对象: %s", g_enemyLabel);

            // B. 敌方 6 大快捷处置矩阵（3列×2行，整齐无溢出）+ 灭霸响指秒杀条
            if (ImGui::BeginTable("##EnemyQuickOps", 3, ImGuiTableFlags_SizingStretchSame))
            {
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (DrawAppliedActionBtn("✓ 百万血沙包 (0攻防/定身)###e_sb", "百万血沙包 (100万血/0攻防)###e_sb",
                                         applied.enemySandbag, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_ENEMY_SANDBAG, g_enemyFilter, 0, 0.0f);
                ItemTip("一键改成 100万血 + 0攻击 + 0双抗 + 定身 + 进蓝门不扣血的极品测伤木桩");

                ImGui::TableSetColumnIndex(1);
                if (DrawAppliedActionBtn("✓ 世界时间静止 (移速归零)###e_frz", "世界时间静止 (移速=0定身)###e_frz",
                                         applied.enemyFrozen, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 6, 0.0f);
                ItemTip("把场上匹配怪物的移速设为 0，原地罚站当靶子");

                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button("解除石化 (恢复移速=1)", ImVec2(-1.0f, 26.0f)))
                    PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 6, 1.0f);
                ItemTip("把被定身的怪物移速恢复为 1.0，继续逛街");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (DrawAppliedActionBtn("✓ 红门观光客 (进蓝门不扣血)###e_uh", "红门观光客 (进蓝门不扣血)###e_uh",
                                         applied.enemyUnharmful, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_ENEMY_UNHARMFUL, g_enemyFilter, 0, 0.0f);
                ItemTip("把目标怪物标记为无害游客，大摇大摆走进蓝门也绝不扣我方生命");

                ImGui::TableSetColumnIndex(1);
                if (DrawAppliedActionBtn("✓ 失重气球 (怪物零重量)###e_wl", "失重气球 (怪物零重量)###e_wl",
                                         applied.enemyWeightless, false, -1.0f, 26.0f))
                    PostRequestEx(REQ_MOD_ENEMY_WEIGHTLESS, g_enemyFilter, 23, 0.0f);
                ItemTip("把怪物重量 (MASS_LEVEL) 抽干成 0，搭配我方「大力出奇迹」让重装大爹当场起飞！");

                ImGui::TableSetColumnIndex(2);
                if (ImGui::Button("全图照妖镜 (破隐匿/迷彩)", ImVec2(-1.0f, 26.0f)))
                    PostRequestEx(REQ_MOD_ENEMY_REVEAL, g_enemyFilter, 0, 0.0f);
                ItemTip("立刻剥离场上匹配怪物的隐匿与迷彩 Buff，让所有老六无处遁形");

                ImGui::EndTable();
            }

            if (DrawDangerBtn("☠ 灭霸响指 · 全图灰飞烟灭 (Unity 主线程安全秒杀匹配怪物)###e_kill", -1.0f, 26.0f))
                PostRequestEx(REQ_MOD_ENEMY_KILL, g_enemyFilter, 0, 0.0f);
            ItemTip("在 Unity 主线程调用原生 FinishWithNoReason 瞬间清场，安全推进波次不卡关");

            ImGui::Separator();

            // C. 怪物属性精调表（2列×3行对齐）
            if (ImGui::BeginTable("##EnemyAttrGrid", 2, ImGuiTableFlags_SizingStretchSame))
            {
                auto drawEnmAttrCell = [&](const char *title, const char *idTag, float *val,
                                           float step, float stepFast, const char *fmt, int attrType) {
                    ImGui::SetNextItemWidth(110.0f);
                    ImGui::InputFloat(idTag, val, step, stepFast, fmt);
                    ImGui::SameLine();
                    bool done = (attrType >= 0 && attrType < 64) && ((applied.enemyAttrMask & (1ULL << attrType)) != 0);
                    char bDone[96], bIdle[96];
                    _snprintf_s(bDone, sizeof(bDone), _TRUNCATE, "✓ %s###ea_%d", title, attrType);
                    _snprintf_s(bIdle, sizeof(bIdle), _TRUNCATE, "%s###ea_%d", title, attrType);
                    if (DrawAppliedActionBtn(bDone, bIdle, done, false, -1.0f, 24.0f))
                        PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, attrType, *val);
                };

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawEnmAttrCell("血条魔改 (改血量)", "##e_hp",  &g_enemyMaxHp,   10000.0f, 100000.0f, "%.0f", 0);
                ImGui::TableSetColumnIndex(1); drawEnmAttrCell("刮痧大师 (改攻击)", "##e_atk", &g_enemyAtk,     100.0f,   500.0f,    "%.0f", 1);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawEnmAttrCell("脆皮纸糊 (改防御)", "##e_def", &g_enemyDef,     100.0f,   500.0f,    "%.0f", 2);
                ImGui::TableSetColumnIndex(1); drawEnmAttrCell("魔抗清零 (改法抗)", "##e_res", &g_enemyMagicRes,10.0f,    20.0f,     "%.0f", 3);

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); drawEnmAttrCell("龟速爬行 (改移速)", "##e_spd", &g_enemyMoveSpd, 0.2f,     0.5f,      "%.2f", 6);
                ImGui::TableSetColumnIndex(1); drawEnmAttrCell("老年挥棒 (改攻速)", "##e_asp", &g_enemyAtkSpd,  20.0f,    50.0f,     "%.0f", 7);

                ImGui::EndTable();
            }

            ImGui::Spacing();

            // D. 本关动态刷怪器（可折叠区块，含 60 只场上上限保护）
            char spHdr[160];
            _snprintf_s(spHdr, sizeof(spHdr), _TRUNCATE,
                        "红门时空裂缝 · 动态刷怪器（本关花名册 %ld 种 · %ld 条路线 · 场上 %d/60）###SpawnHdr",
                        (long)g_stageEnemyCount, (long)g_stageRouteCount, localEnemyTotal);
            if (ImGui::CollapsingHeader(spHdr, ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (ImGui::SmallButton("重新破译花名册(重读本关怪物)"))
                    PostRequestEx(REQ_SCAN_STAGE_ENEMIES, "", 0, 0.0f);

                const int ecnt = (int)g_stageEnemyCount;
                for (int i = 0; i < ecnt; ++i)
                {
                    if (i > 0 && (i % 4) != 0) ImGui::SameLine();
                    bool isSel = (strcmp(g_spawnEnemyKey, g_stageEnemies[i].id) == 0);
                    if (isSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.35f, 1.0f));
                    char btn[180];
                    if (g_stageEnemies[i].name[0])
                        _snprintf_s(btn, sizeof(btn), _TRUNCATE, "%s###se%d", g_stageEnemies[i].name, i);
                    else
                        _snprintf_s(btn, sizeof(btn), _TRUNCATE, "%s###se%d", g_stageEnemies[i].id, i);
                    if (ImGui::SmallButton(btn))
                        CopyStrSafe(g_spawnEnemyKey, sizeof(g_spawnEnemyKey), g_stageEnemies[i].id);
                    if (isSel) ImGui::PopStyleColor();
                }

                ImGui::SetNextItemWidth(175.0f);
                ImGui::InputTextWithHint("##spKey", "怪物ID", g_spawnEnemyKey, sizeof(g_spawnEnemyKey));
                ImGui::SameLine();
                ImGui::SetNextItemWidth(64.0f);
                ImGui::InputInt("路线", &g_spawnRouteIdx);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(64.0f);
                ImGui::InputInt("数量", &g_spawnCount);
                if (g_spawnCount < 1) g_spawnCount = 1;
                if (g_spawnCount > 20) g_spawnCount = 20;
                ImGui::SameLine();
                ImGui::Checkbox("出生即魔改属性", &g_spawnApplyCustom);
                ImGui::SameLine();
                if (DrawAppliedActionBtn("★ 撕开裂缝刷出###spGo", "★ 撕开裂缝刷出(刷怪)###spGo", false, true, 168.0f, 24.0f))
                    PostRequestEx(REQ_SPAWN_ENEMY, g_spawnEnemyKey, 0, 0.0f, g_spawnRouteIdx, g_spawnCount);
            }

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();

    // ========================================================================
    // 4. 底部默认折叠的施法历史抽屉（需要时点开看，不占主视野）
    // ========================================================================
    ImGui::Spacing();
    char histHdr[80];
    _snprintf_s(histHdr, sizeof(histHdr), _TRUNCATE, "黑科技施法记录 (%d/8)###HistDrawer", histCount);
    if (ImGui::CollapsingHeader(histHdr))
    {
        if (histCount == 0)
        {
            ImGui::TextDisabled("暂无施法记录，快去按几个按钮整活吧！");
        }
        else
        {
            for (int i = 0; i < histCount; ++i)
            {
                ImGui::TextColored(FeedbackColor(histCopy[i].level), "%s %s  %s",
                                   FeedbackPrefix(histCopy[i].level),
                                   histCopy[i].timeStr,
                                   histCopy[i].msg);
            }
        }
    }
}

// 渲染线程，在主面板 TabBar 首位渲染「★ 物理法则改写」+ 收拢后的「博士实验室 (开发工具)」
void Mod_DrawPanelTabs()
{
    if (ImGui::BeginTabItem("★ 物理法则改写 (战斗定制)"))
    {
        DrawBattleCustomizerTab();
        ImGui::EndTabItem();
    }

    if (ImGui::BeginTabItem("博士实验室 (开发工具)"))
    {
        if (ImGui::BeginTabBar("##DevToolsSubTabs"))
        {
            if (ImGui::BeginTabItem("类浏览器"))
            {
                ArkPanelClassBrowser();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("对象检查"))
            {
                ArkPanelObjects();
                ImGui::EndTabItem();
            }
            char watchTab[48];
            _snprintf_s(watchTab, sizeof(watchTab), _TRUNCATE, "字段监视 (%d)###WatchTab", ArkWatchCount());
            if (ImGui::BeginTabItem(watchTab))
            {
                ArkPanelWatch();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("方法调用"))
            {
                ArkPanelInvoke();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("导出Dumper"))
            {
                ArkPanelDumper();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("快捷键"))
            {
                ArkPanelHotkeys();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("类结构速导"))
            {
                ImGui::TextDisabled("快速将指定类结构导出到 il2cpp_mod.log");
                static char clsBuf[128] = { 0 };
                ImGui::SetNextItemWidth(320.0f);
                ImGui::InputTextWithHint("##cls", "命名空间.类名 (如 Torappu.Battle.BattleController)", clsBuf, sizeof(clsBuf));
                ImGui::SameLine();
                ImGui::BeginDisabled(!clsBuf[0]);
                if (ImGui::Button("Dump 类"))
                    PostRequestEx(REQ_DUMP_CLASS, clsBuf, 0, 0.0f);
                ImGui::EndDisabled();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndTabItem();
    }
}

void Mod_OnShutdown()
{
    Log("[user] Mod_OnShutdown —— 保存配置并安全撤电清理");
    InterlockedExchange(&g_lockInfAmmo, 0);
    InterlockedExchange(&g_lockInfSp, 0);
    InterlockedExchange(&g_lockGodHp, 0);
    InterlockedExchange(&g_lockBattleDp, 0);
    InterlockedExchange(&g_lockImmune, 0);
    InterlockedExchange(&g_ruleAllTerrain, 0);
    InterlockedExchange(&g_ruleOverlap, 0);
    InterlockedExchange(&g_ruleGlobalRange, 0);
    InterlockedExchange(&g_ruleIgnoreMotion, 0);
    g_tileBackupCount     = 0;
    g_cardCondBackupCount = 0;
    g_rangeBackupCount    = 0;
    g_motionBackupCount   = 0;
    if (ArkCfgTakeDirty())
        ArkCfgSave();
}
