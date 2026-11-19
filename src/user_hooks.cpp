// user_hooks.cpp — 6 大内置分析面板 + 明日方舟战斗定制工作台（干员 / 弹药 / 怪物 / 刷怪 / 持续锁定 / 全局控制）
// ============================================================================
// 线程分工：
//   · Mod_DrawPanelTabs()  渲染线程 (Present)      -> 只画 ImGui + 写请求槽 PostRequestEx()
//   · Mod_OnTick()         Worker 线程 (每 100ms)  -> 跑请求、自动扫描本局编队/怪物、持续锁定、Watch 刷新
//   · Mod_OnGameThread()   Unity 主线程 (ExitWndProc 收到 WM_APP+0x4D4F)
//                          -> 跑 ArkMainTaskPump()，执行刷怪 / 战斗倍速等需主线程调用的 Unity 接口
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
        REQ_RESET_SESSION,        // 一键复位本局状态（关全锁 + 选全体 + 恢复 1x 速度 + 刷快照）
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
        REQ_SPAWN_ENEMY,          // 在本关动态刷出指定怪物（Worker 拿控制器 -> 主线程生成）
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

    // 主线程刷怪上下文
    struct MainSpawnCtx
    {
        unsigned int bcHandle;
        char         enemyKey[128];
        int          routeIdx;
        int          count;
        int          applyCustom;
        float        maxHp;
        float        atk;
        float        def;
        float        magicRes;
        float        moveSpd;
    };

    // 主线程调速上下文
    struct MainSpeedCtx
    {
        unsigned int bcHandle;
        int          mode;        // 0 = SpeedLevel 枚举, 1 = 自定义 timeScale
        int          speedLevel;
        float        timeScale;
    };

    Request       g_req;
    volatile LONG g_reqSeq       = 0;
    volatile LONG g_reqDone      = 0;
    unsigned      g_tickCounter  = 0;
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

    // 反馈横幅 & 最近 8 条操作记录
    FeedbackEntry g_lastFeedback = { FB_INFO, "", "就绪：进入任意战斗关卡后，上方会自动列出本局带入的全部干员与怪物" };
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
    char  g_selectedCharLabel[128]= "全体编队干员";
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
    int   g_charAmmoTarget        = 99;

    // 持续自动锁定开关（每 100ms 在 Worker 线程自动维持）
    volatile LONG g_lockInfAmmo   = 0;
    volatile LONG g_lockInfSp     = 0;
    volatile LONG g_lockGodHp     = 0;
    volatile LONG g_lockBattleDp  = 0;

    // ---- 2. 敌方怪物定制 & 刷怪 UI 状态 ----
    char  g_enemyFilter[128]      = "";      // 空 = 场上全体敌人；点选上方怪物按钮或输入中文/ID 筛选
    char  g_enemyLabel[128]       = "全场所有怪物";
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

    MainSpawnCtx  g_mainSpawnCtx;
    volatile LONG g_mainSpawnBusy = 0;
    MainSpeedCtx  g_mainSpeedCtx;
    volatile LONG g_mainSpeedBusy = 0;
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
    CopyStrSafe(g_req.text, sizeof(g_req.text), text);
    g_req.attrType  = attrType;
    g_req.val       = val;
    g_req.extraInt1 = ex1;
    g_req.extraInt2 = ex2;
    InterlockedExchange(&g_req.kind, (LONG)kind);
    InterlockedIncrement(&g_reqSeq);
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

    // 路径 2：扫描归属于该干员的 AbilityEventCounter（捕获特性/天赋/过载附加弹药计数器）
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

static int ApplyCharacterAmmo(const char *filter, int32_t targetAmmo, bool changeMax,
                              int *outCharMatched, char *lastMatched, int cap,
                              int32_t *outBRem, int32_t *outBMax, int32_t *outARem, int32_t *outAMax)
{
    Il2CppApi &api = ir::Api();
    if (outCharMatched) *outCharMatched = 0;

    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!ce || !ce->klass) return -1;

    Il2CppObject *chars[256];
    int n = ArkGcEnumObjects(ce->klass, chars, 256, 1);

    Il2CppObject *allCounters[128];
    int allCnt = 0;
    if (const ArkClassEntry *cntCe = ArkReflectFindClass("Torappu.Battle.Abilities.AbilityEventCounter"))
    {
        if (cntCe->klass)
            allCnt = ArkGcEnumObjects(cntCe->klass, allCounters, 128, 1);
    }

    FieldInfo *fData = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    int modified = 0;
    int matchedChars = 0;

    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *chr = chars[i];
        if (!chr || !fData || !IsEntityAlive(chr, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, chr), filter, desc, sizeof(desc)))
            continue;
        ++matchedChars;

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
    Il2CppObject *bcs[4] = { nullptr };
    if (ArkGcEnumObjects(bcCe->klass, bcs, 4, 1) > 0)
        return bcs[0];
    return nullptr;
}

// 零 GC 停顿读取 BattleController 当前阶段/代理指挥状态/关卡ID/逻辑帧
static void ReadBattleControllerMeta(Il2CppObject *bc, int32_t *outState, bool *outAutoReplay,
                                     char *outLevelId, int lvlCap, uint32_t *outFrameCnt)
{
    if (outState)      *outState = 0;
    if (outAutoReplay) *outAutoReplay = false;
    if (outLevelId && lvlCap > 0) outLevelId[0] = 0;
    if (outFrameCnt)   *outFrameCnt = 0;
    if (!bc) return;

    Il2CppApi &api = ir::Api();
    Il2CppClass *bk = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bk) return;

    if (outState)
    {
        if (FieldInfo *fSt = ArkReflectFindField(bk, "m_state", nullptr))
            if (void *a = ArkReflectFieldAddr(fSt, bc)) *outState = *(int32_t *)a;
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

// 清理上一局残留状态（防脏目标/脏怪物ID/脏持续锁定带入下一场对战）
static void ResetPerBattleState(bool clearLocks)
{
    if (clearLocks)
    {
        InterlockedExchange(&g_lockInfAmmo, 0);
        InterlockedExchange(&g_lockInfSp, 0);
        InterlockedExchange(&g_lockGodHp, 0);
        InterlockedExchange(&g_lockBattleDp, 0);
    }
    SpinLock(&g_snapLock);
    g_selectedCharId[0] = 0;
    CopyStrSafe(g_selectedCharLabel, sizeof(g_selectedCharLabel), "全体编队干员");
    g_charSearch[0] = 0;
    g_enemyFilter[0] = 0;
    CopyStrSafe(g_enemyLabel, sizeof(g_enemyLabel), "全场所有怪物");
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
    return g_lockInfAmmo || g_lockInfSp || g_lockGodHp || g_lockBattleDp;
}

static void RefreshBattleSnapshot();

// 对战生命周期与脏状态守卫（每 100ms 由 Worker 线程运行，零 GC 停顿）
static void PollBattleSessionGuard()
{
    Il2CppObject *bc = GetBattleControllerFast();
    int32_t st = 0;
    bool autoRep = false;
    char lvlId[96] = { 0 };
    uint32_t frameCnt = 0;
    if (bc)
        ReadBattleControllerMeta(bc, &st, &autoRep, lvlId, sizeof(lvlId), &frameCnt);

    BattlePhase newPhase = PHASE_LOBBY;
    if (bc)
    {
        if (st == 2)      newPhase = PHASE_PLAYING;
        else if (st == 3) newPhase = PHASE_FINISHED;
        else              newPhase = PHASE_LOADING;
    }

    const BattlePhase prevPhase = (BattlePhase)InterlockedExchange(&g_battlePhase, (LONG)newPhase);
    const bool prevAutoRep = (InterlockedExchange(&g_isAutoReplay, autoRep ? 1 : 0) != 0);
    InterlockedExchange(&g_inBattle, (newPhase == PHASE_PLAYING) ? 1 : 0);

    // 1) 战斗结束或返回大厅 -> 立即停止脏锁定并清理本局快照缓存
    if (prevPhase == PHASE_PLAYING && (newPhase == PHASE_FINISHED || newPhase == PHASE_LOBBY))
    {
        const bool hadLocks = HasAnyLockActive();
        const bool autoClr = (InterlockedCompareExchange(&g_autoResetNew, 0, 0) != 0);
        ResetPerBattleState(autoClr);
        if (newPhase == PHASE_FINISHED)
            SetFeedback(FB_INFO, hadLocks && autoClr
                        ? "本场战斗已结算：已自动关闭全部持续锁定并清理本局缓存"
                        : "本场战斗已结算：修改功能已自动锁定，进入下一关后重新解锁");
        else
            SetFeedback(FB_INFO, hadLocks && autoClr
                        ? "已返回主界面：已自动关闭持续锁定并重置目标，防止脏状态带入下一局"
                        : "已返回主界面：Mod 保持待命，进入任意战斗关卡后自动解锁");
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
                SetFeedback(FB_WARN, "已进入第 #%ld 局 [%s]（代理指挥录像中）：为防破坏代理轴，已自动关闭所有持续锁定！",
                            (long)sid, lvlId[0] ? lvlId : "战斗关卡");
            else
                SetFeedback(FB_OK, "已进入第 #%ld 局 [%s]：战斗定制已解锁，并已自动清理上一局目标与残留状态",
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
                SetFeedback(FB_WARN, "检测到切换为代理指挥模式：已自动暂停持续锁定，避免影响录像结算");
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

    // 1) 扫描手牌编队 (Deck.CharacterCard)
    if (const ArkClassEntry *cardCe = ArkReflectFindClass("CharacterCard"))
    {
        if (cardCe->klass)
        {
            Il2CppObject *cards[128];
            int cn = ArkGcEnumObjects(cardCe->klass, cards, 128, 1);
            FieldInfo *fData  = ArkReflectFindField(cardCe->klass, "m_data", nullptr);
            FieldInfo *fDelta = ArkReflectFindField(cardCe->klass, "m_costDelta", nullptr);
            FieldInfo *fSnap  = ArkReflectFindField(cardCe->klass, "<attributes>k__BackingField", nullptr);

            for (int i = 0; i < cn; ++i)
            {
                Il2CppObject *card = cards[i];
                if (!card || !fData) continue;
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
    }

    // 2) 扫描场上存活干员 (Torappu.Battle.Character) + 弹药计数器
    Il2CppObject *allCounters[128];
    int allCnt = 0;
    if (const ArkClassEntry *cntCe = ArkReflectFindClass("Torappu.Battle.Abilities.AbilityEventCounter"))
    {
        if (cntCe->klass)
            allCnt = ArkGcEnumObjects(cntCe->klass, allCounters, 128, 1);
    }

    if (const ArkClassEntry *charCe = ArkReflectFindClass("Torappu.Battle.Character"))
    {
        if (charCe->klass)
        {
            Il2CppObject *chars[256];
            int n = ArkGcEnumObjects(charCe->klass, chars, 256, 1);
            FieldInfo *fData  = ArkReflectFindField(charCe->klass, "<data>k__BackingField", nullptr);
            FieldInfo *fAttr  = ArkReflectFindField(charCe->klass, "m_attributes", nullptr);
            FieldInfo *fHp    = ArkReflectFindField(charCe->klass, "m_hp", nullptr);
            FieldInfo *fSp    = ArkReflectFindField(charCe->klass, "m_sp", nullptr);
            FieldInfo *fMaxSp = ArkReflectFindField(charCe->klass, "<maxSp>k__BackingField", nullptr);

            for (int i = 0; i < n; ++i)
            {
                Il2CppObject *chr = chars[i];
                if (!chr || !fData || !IsEntityAlive(chr, charCe->klass)) continue;
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
                if (FindCharAmmoCounters(chr, charCe->klass, myCnts, 4, allCounters, allCnt) > 0)
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
    }

    // 3) 扫描场上存活怪物 (Torappu.Battle.Enemy)
    LiveEnemyGroup tmpEnemies[32];
    int eGrpCount = 0;
    int eTotal = 0;
    if (const ArkClassEntry *eCe = ArkReflectFindClass("Torappu.Battle.Enemy"))
    {
        if (eCe->klass)
        {
            Il2CppObject *enms[256];
            int en = ArkGcEnumObjects(eCe->klass, enms, 256, 1);
            FieldInfo *fAttr = ArkReflectFindField(eCe->klass, "m_attributes", nullptr);
            FieldInfo *fHp   = ArkReflectFindField(eCe->klass, "m_hp", nullptr);

            for (int i = 0; i < en; ++i)
            {
                Il2CppObject *enm = enms[i];
                if (!enm || !IsEntityAlive(enm, eCe->klass)) continue;
                char keyBuf[96] = { 0 }, nmBuf[96] = { 0 };
                if (!ExtractEnemyKeyName(enm, eCe->klass, keyBuf, sizeof(keyBuf), nmBuf, sizeof(nmBuf)))
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
// 各项修改执行函数（运行在 Worker 线程）
// ============================================================================
static int ApplyCardCostDelta(const char *filter, int32_t costDelta, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("CharacterCard");
    if (!ce || !ce->klass) return -1;

    Il2CppObject *objs[128];
    int n = ArkGcEnumObjects(ce->klass, objs, 128, 1);
    if (n <= 0) return 0;

    FieldInfo *fData  = ArkReflectFindField(ce->klass, "m_data", nullptr);
    FieldInfo *fDelta = ArkReflectFindField(ce->klass, "m_costDelta", nullptr);
    FieldInfo *fSnap  = ArkReflectFindField(ce->klass, "<attributes>k__BackingField", nullptr);
    FieldInfo *fDeck  = ArkReflectFindField(ce->klass, "m_deck", nullptr);
    if (!fData || !fDelta) return -2;

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *card = objs[i];
        if (!card) continue;
        Il2CppObject *dataObj = api.il2cpp_field_get_value_object(fData, card);
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(dataObj, filter, desc, sizeof(desc)))
            continue;

        ArkReflectWriteFieldNum(fDelta, card, (double)costDelta);

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
                            if (costDelta <= -50)
                                OverwriteAttributeOnObj(innerAttr, 4 /* COST */, 0.0f);
                        }
                    }
                    if (MethodInfo *mModCost = api.il2cpp_class_get_method_from_name(sk, "ModifyCost", 1))
                    {
                        int32_t targetCost = (costDelta <= -50) ? 0 : costDelta;
                        void *cargs[1] = { &targetCost };
                        Il2CppException *exc = nullptr;
                        api.il2cpp_runtime_invoke(mModCost, snap, cargs, &exc);
                    }
                }
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
    return hit;
}

static int ApplyCardResetRespawn(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("CharacterCard");
    if (!ce || !ce->klass) return -1;

    Il2CppObject *objs[128];
    int n = ArkGcEnumObjects(ce->klass, objs, 128, 1);
    if (n <= 0) return 0;

    FieldInfo *fData = ArkReflectFindField(ce->klass, "m_data", nullptr);
    FieldInfo *fSnap = ArkReflectFindField(ce->klass, "<attributes>k__BackingField", nullptr);
    MethodInfo *mReset = api.il2cpp_class_get_method_from_name(ce->klass, "ResetRespawnTime", 1);
    if (!mReset)
    {
        if (Il2CppClass *parent = api.il2cpp_class_get_parent(ce->klass))
            mReset = api.il2cpp_class_get_method_from_name(parent, "ResetRespawnTime", 1);
    }

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *card = objs[i];
        if (!card || !fData) continue;
        char desc[128] = { 0 };
        if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, card), filter, desc, sizeof(desc)))
            continue;

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
                            OverwriteAttributeOnObj(innerAttr, 21 /* RESPAWN_TIME */, 1.0f);
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
        ++hit;
    }
    return hit;
}

static int ApplyFreeDeployLimit(const char *filter)
{
    Il2CppApi &api = ir::Api();
    int hit = 0;

    if (const ArkClassEntry *ce = ArkReflectFindClass("CharacterCard"))
    {
        if (ce->klass)
        {
            Il2CppObject *objs[128];
            int n = ArkGcEnumObjects(ce->klass, objs, 128, 1);
            FieldInfo *fData  = ArkReflectFindField(ce->klass, "m_data", nullptr);
            FieldInfo *fDont1 = ArkReflectFindField(ce->klass, "m_dontOccupyDeployCnt", nullptr);
            FieldInfo *fDont2 = ArkReflectFindField(ce->klass, "m_dontOccupyMaxDeployCnt", nullptr);
            for (int i = 0; i < n; ++i)
            {
                Il2CppObject *card = objs[i];
                if (!card || !fData) continue;
                char desc[128] = { 0 };
                if (!CheckCharDataMatches(api.il2cpp_field_get_value_object(fData, card), filter, desc, sizeof(desc)))
                    continue;
                if (fDont1) ArkReflectWriteFieldNum(fDont1, card, 1.0);
                if (fDont2) ArkReflectWriteFieldNum(fDont2, card, 1.0);
                ++hit;
            }
        }
    }

    if (const ArkClassEntry *bcCe = ArkReflectFindClass("Torappu.Battle.BattleController"))
    {
        if (bcCe->klass)
        {
            Il2CppObject *bcs[8];
            if (ArkGcEnumObjects(bcCe->klass, bcs, 8, 1) > 0 && bcs[0])
            {
                if (MethodInfo *mModLim = api.il2cpp_class_get_method_from_name(bcCe->klass, "ModifyCharacterLimit", 3))
                {
                    int32_t addVal = 99;
                    Il2CppObject *src = nullptr;
                    int32_t side = 0;
                    void *args[3] = { &addVal, src, &side };
                    Il2CppException *exc = nullptr;
                    api.il2cpp_runtime_invoke(mModLim, bcs[0], args, &exc);
                }
            }
        }
    }
    return hit;
}

static int ApplyCharacterAttribute(const char *filter, int32_t attrType, float val,
                                   char *lastMatched, int cap, float *outBeforeVal = nullptr)
{
    Il2CppApi &api = ir::Api();
    int hit = 0;

    if (const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character"))
    {
        if (ce->klass)
        {
            Il2CppObject *objs[256];
            int n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
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
                if (outBeforeVal && hit == 0 && attrObj)
                    *outBeforeVal = ReadAttributeOnObj(attrObj, attrType);

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

    // 同步写入手牌快照
    if (const ArkClassEntry *cardCe = ArkReflectFindClass("CharacterCard"))
    {
        if (cardCe->klass)
        {
            Il2CppObject *cards[128];
            int cn = ArkGcEnumObjects(cardCe->klass, cards, 128, 1);
            FieldInfo *fData = ArkReflectFindField(cardCe->klass, "m_data", nullptr);
            FieldInfo *fSnap = ArkReflectFindField(cardCe->klass, "<attributes>k__BackingField", nullptr);
            for (int i = 0; i < cn; ++i)
            {
                Il2CppObject *card = cards[i];
                if (!card || !fData || !fSnap) continue;
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
                        if (OverwriteAttributeOnObj(innerAttr, attrType, val) && hit == 0)
                            CopyStrSafe(lastMatched, cap, desc);
                    }
                }
            }
        }
    }
    return hit;
}

static int ApplyCharacterFullHpSp(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!ce || !ce->klass) return 0;

    Il2CppObject *objs[256];
    int n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
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

static bool ApplyBattleDp(int32_t targetDp)
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    if (!bc) return false;
    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bcKlass) return false;

    int32_t side = 0;
    if (MethodInfo *mSetMax = api.il2cpp_class_get_method_from_name(bcKlass, "SetMaxCost", 2))
    {
        int32_t maxDp = (targetDp > 999) ? targetDp : 999;
        void *args[2] = { &maxDp, &side };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mSetMax, bc, args, &exc);
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

static bool ApplyBattleLifePoint(int32_t targetLife)
{
    Il2CppApi &api = ir::Api();
    Il2CppObject *bc = GetBattleControllerFast();
    if (!bc) return false;
    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    if (!bcKlass) return false;

    if (MethodInfo *mSetLp = api.il2cpp_class_get_method_from_name(bcKlass, "SetLifePoint", 3))
    {
        int32_t side = 0;
        uint8_t force = 1;
        void *args[3] = { &targetLife, &side, &force };
        Il2CppException *exc = nullptr;
        api.il2cpp_runtime_invoke(mSetLp, bc, args, &exc);
        return exc == nullptr;
    }
    return false;
}

static int ApplyEnemyAttribute(const char *filter, int32_t attrType, float val,
                               char *lastMatched, int cap, float *outBeforeVal = nullptr)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass) return -1;

    Il2CppObject *objs[256];
    int n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
    FieldInfo *fAttr = ArkReflectFindField(ce->klass, "m_attributes", nullptr);
    if (!fAttr) return -2;

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *enm = objs[i];
        if (!enm || !IsEntityAlive(enm, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckEnemyMatches(enm, ce->klass, filter, desc, sizeof(desc)))
            continue;

        Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, enm);
        if (outBeforeVal && hit == 0 && attrObj)
            *outBeforeVal = ReadAttributeOnObj(attrObj, attrType);

        if (OverwriteAttributeOnObj(attrObj, attrType, val))
        {
            if (attrType == 0 /* MAX_HP */)
                SetEntityCurrentHp(enm, ce->klass, val);
            CopyStrSafe(lastMatched, cap, desc);
            ++hit;
        }
    }
    return hit;
}

static int ApplyEnemyUnharmful(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass) return 0;

    Il2CppObject *objs[256];
    int n = ArkGcEnumObjects(ce->klass, objs, 256, 1);
    FieldInfo *fLpRed = ArkReflectFindField(ce->klass, "<lifePointReduce>k__BackingField", nullptr);
    MethodInfo *mUnharm = api.il2cpp_class_get_method_from_name(ce->klass, "MarkUnharmful", 0);

    int hit = 0;
    for (int i = 0; i < n; ++i)
    {
        Il2CppObject *enm = objs[i];
        if (!enm || !IsEntityAlive(enm, ce->klass)) continue;
        char desc[128] = { 0 };
        if (!CheckEnemyMatches(enm, ce->klass, filter, desc, sizeof(desc)))
            continue;

        if (fLpRed) ArkReflectWriteFieldNum(fLpRed, enm, 0.0);
        if (mUnharm)
        {
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mUnharm, enm, nullptr, &exc);
        }
        CopyStrSafe(lastMatched, cap, desc);
        ++hit;
    }
    return hit;
}

static int ApplyEnemyKill(const char *filter, char *lastMatched, int cap)
{
    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Enemy");
    if (!ce || !ce->klass) return 0;

    Il2CppObject *objs[256];
    int n = ArkGcEnumObjects(ce->klass, objs, 256, 1);

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
// 持续自动锁定驱动（每 100ms 由 Worker 线程执行，仅战斗中生效）
// ============================================================================
static void PumpPersistentLocks()
{
    if (InterlockedCompareExchange(&g_battlePhase, 0, 0) != PHASE_PLAYING)
        return;

    const bool lockAmmo = InterlockedCompareExchange(&g_lockInfAmmo, 0, 0) != 0;
    const bool lockSp   = InterlockedCompareExchange(&g_lockInfSp, 0, 0) != 0;
    const bool lockHp   = InterlockedCompareExchange(&g_lockGodHp, 0, 0) != 0;

    if (!lockAmmo && !lockSp && !lockHp)
        return;

    Il2CppApi &api = ir::Api();
    const ArkClassEntry *ce = ArkReflectFindClass("Torappu.Battle.Character");
    if (!ce || !ce->klass) return;

    Il2CppObject *chars[256];
    int n = ArkGcEnumObjects(ce->klass, chars, 256, 1);
    FieldInfo *fData = ArkReflectFindField(ce->klass, "<data>k__BackingField", nullptr);
    FieldInfo *fAttr = ArkReflectFindField(ce->klass, "m_attributes", nullptr);

    Il2CppObject *allCounters[128];
    int allCnt = 0;
    if (lockAmmo)
    {
        if (const ArkClassEntry *cntCe = ArkReflectFindClass("Torappu.Battle.Abilities.AbilityEventCounter"))
            if (cntCe->klass) allCnt = ArkGcEnumObjects(cntCe->klass, allCounters, 128, 1);
    }

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
            if (fAttr)
            {
                if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, chr))
                    maxHp = ReadAttributeOnObj(attrObj, 0);
            }
            if (maxHp > 0.0f)
                SetEntityCurrentHp(chr, ce->klass, maxHp);
        }
        if (lockSp)
        {
            FillEntitySp(chr, ce->klass);
        }
        if (lockAmmo)
        {
            Il2CppObject *myCnts[8];
            int k = FindCharAmmoCounters(chr, ce->klass, myCnts, 8, allCounters, allCnt);
            for (int j = 0; j < k; ++j)
            {
                Il2CppClass *cc = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(myCnts[j]) : myCnts[j]->klass;
                if (!cc) continue;
                if (FieldInfo *fEvt = ArkReflectFindField(cc, "m_eventCount", nullptr))
                    if (void *a = ArkReflectFieldAddr(fEvt, myCnts[j])) *(int32_t *)a = 0;
                if (FieldInfo *fNot = ArkReflectFindField(cc, "m_notCountNext", nullptr))
                    if (void *a = ArkReflectFieldAddr(fNot, myCnts[j])) *(uint8_t *)a = 1;
                if (FieldInfo *fRdy = ArkReflectFindField(cc, "m_readyToReset", nullptr))
                    if (void *a = ArkReflectFieldAddr(fRdy, myCnts[j])) *(uint8_t *)a = 0;
            }
        }
    }
}

// ============================================================================
// Unity 主线程专属任务：动态刷怪 & 战斗倍速
// ============================================================================
static void MainThreadSpawnEnemyTask(void *ud)
{
    MainSpawnCtx *ctx = (MainSpawnCtx *)ud;
    if (!ctx) return;

    Il2CppApi &api = ir::Api();
    ir::EnsureAttached();

    Il2CppObject *bc = ctx->bcHandle ? ArkGcTarget(ctx->bcHandle) : nullptr;
    int32_t bcState = 0;
    if (bc) ReadBattleControllerMeta(bc, &bcState, nullptr, nullptr, 0, nullptr);
    if (!bc || bc != GetBattleControllerFast() || bcState != 2)
    {
        if (ctx->bcHandle) ArkGcUnpin(ctx->bcHandle);
        SetFeedback(FB_WARN, "刷怪已取消：当前战斗已结束或尚未进入 PLAYING 状态");
        InterlockedExchange(&g_mainSpawnBusy, 0);
        return;
    }

    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    FieldInfo *fLvl = ArkReflectFindField(bcKlass, "m_levelData", nullptr);
    MethodInfo *mGetSched = api.il2cpp_class_get_method_from_name(bcKlass, "get_scheduler", 0);
    if (!fLvl || !mGetSched)
    {
        ArkGcUnpin(ctx->bcHandle);
        SetFeedback(FB_ERR, "刷怪失败：找不到 m_levelData 或 get_scheduler");
        InterlockedExchange(&g_mainSpawnBusy, 0);
        return;
    }

    Il2CppObject *lvlObj = api.il2cpp_field_get_value_object(fLvl, bc);
    Il2CppException *exc = nullptr;
    Il2CppObject *schedObj = api.il2cpp_runtime_invoke(mGetSched, bc, nullptr, &exc);
    if (!lvlObj || !schedObj || exc)
    {
        ArkGcUnpin(ctx->bcHandle);
        SetFeedback(FB_WARN, "刷怪失败：Scheduler 或 LevelData 为空，请确认战斗已开始");
        InterlockedExchange(&g_mainSpawnBusy, 0);
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
                int startIdx = (ctx->routeIdx >= 0) ? (ctx->routeIdx % rLen) : 0;
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
        ArkGcUnpin(ctx->bcHandle);
        SetFeedback(FB_ERR, "刷怪失败：找不到合法路线或 SpawnEnemyWithRoute 方法");
        InterlockedExchange(&g_mainSpawnBusy, 0);
        return;
    }

    Il2CppString *keyStr = api.il2cpp_string_new(ctx->enemyKey);
    int spawned = 0;
    int total = (ctx->count < 1) ? 1 : (ctx->count > 20 ? 20 : ctx->count);

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
            if (ctx->applyCustom)
            {
                Il2CppClass *ek = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(newEnemy) : newEnemy->klass;
                if (FieldInfo *fAttr = ArkReflectFindField(ek, "m_attributes", nullptr))
                {
                    if (Il2CppObject *attrObj = api.il2cpp_field_get_value_object(fAttr, newEnemy))
                    {
                        OverwriteAttributeOnObj(attrObj, 0, ctx->maxHp);
                        OverwriteAttributeOnObj(attrObj, 1, ctx->atk);
                        OverwriteAttributeOnObj(attrObj, 2, ctx->def);
                        OverwriteAttributeOnObj(attrObj, 3, ctx->magicRes);
                        OverwriteAttributeOnObj(attrObj, 6, ctx->moveSpd);
                        SetEntityCurrentHp(newEnemy, ek, ctx->maxHp);
                    }
                }
            }
        }
    }

    ArkGcUnpin(ctx->bcHandle);
    if (spawned > 0)
        SetFeedback(FB_OK, "已在路线 #%d 刷出 %d/%d 只 \"%s\"%s",
                    usedRouteIdx, spawned, total, ctx->enemyKey,
                    ctx->applyCustom ? "（已套用自定义血量/攻防/移速）" : "");
    else
        SetFeedback(FB_WARN, "刷怪未生效：\"%s\" 不在本关预加载怪物库中，请点选上方本关怪物按钮", ctx->enemyKey);

    InterlockedExchange(&g_mainSpawnBusy, 0);
}

static void MainThreadBattleSpeedTask(void *ud)
{
    MainSpeedCtx *ctx = (MainSpeedCtx *)ud;
    if (!ctx) return;

    Il2CppApi &api = ir::Api();
    ir::EnsureAttached();

    Il2CppObject *bc = ctx->bcHandle ? ArkGcTarget(ctx->bcHandle) : nullptr;
    int32_t bcState = 0;
    if (bc) ReadBattleControllerMeta(bc, &bcState, nullptr, nullptr, 0, nullptr);
    if (!bc || bc != GetBattleControllerFast() || bcState != 2)
    {
        if (ctx->bcHandle) ArkGcUnpin(ctx->bcHandle);
        SetFeedback(FB_WARN, "调速已取消：当前战斗已结束或尚未开始");
        InterlockedExchange(&g_mainSpeedBusy, 0);
        return;
    }

    Il2CppClass *bcKlass = api.il2cpp_object_get_class ? api.il2cpp_object_get_class(bc) : bc->klass;
    bool ok = false;
    if (ctx->mode == 0)
    {
        if (MethodInfo *mSetLvl = api.il2cpp_class_get_method_from_name(bcKlass, "set_speedLevel", 1))
        {
            int32_t lvl = ctx->speedLevel;
            void *args[1] = { &lvl };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetLvl, bc, args, &exc);
            ok = (exc == nullptr);
        }
        const char *lvlNames[4] = { "慢速 (SLOW_MOTION)", "1x 标准", "2x 加速", "极速 (SUPER_FAST)" };
        const char *ln = (ctx->speedLevel >= 0 && ctx->speedLevel < 4) ? lvlNames[ctx->speedLevel] : "未知档位";
        if (ok)
            SetFeedback(FB_OK, "已将战斗原生速度档位切换为：%s", ln);
        else
            SetFeedback(FB_ERR, "战斗速度档位切换失败（set_speedLevel 异常）");
    }
    else
    {
        if (MethodInfo *mSetTs = api.il2cpp_class_get_method_from_name(bcKlass, "set_timeScale", 1))
        {
            float ts = ctx->timeScale;
            void *args[1] = { &ts };
            Il2CppException *exc = nullptr;
            api.il2cpp_runtime_invoke(mSetTs, bc, args, &exc);
            ok = (exc == nullptr);
        }
        if (ok)
            SetFeedback(FB_OK, "已将战场时间倍率 (timeScale) 设为 %.2fx", ctx->timeScale);
        else
            SetFeedback(FB_ERR, "自定义时间倍率设置失败");
    }

    ArkGcUnpin(ctx->bcHandle);
    InterlockedExchange(&g_mainSpeedBusy, 0);
}

// ============================================================================
// 请求消费主循环
// ============================================================================
static void ConsumeRequest()
{
    LONG kind = InterlockedExchange(&g_req.kind, REQ_NONE);
    if (kind == REQ_NONE) return;

    char matched[128] = { 0 };
    bool needRefreshSnap = true;

    // 统一阶段守卫：非战斗进行中时拦截一切战斗修改请求，给出精确时机指引，杜绝脏写
    const BattlePhase curPhase = (BattlePhase)InterlockedCompareExchange(&g_battlePhase, 0, 0);
    if (kind != REQ_DUMP_CLASS && kind != REQ_SCAN_ROSTER && kind != REQ_RESET_SESSION && curPhase != PHASE_PLAYING)
    {
        if (curPhase == PHASE_LOBBY)
            SetFeedback(FB_WARN, "操作已拦截：当前位于主界面/大厅，请先进入任意战斗关卡后再使用战斗定制功能");
        else if (curPhase == PHASE_LOADING)
            SetFeedback(FB_WARN, "操作已拦截：关卡正在初始化加载中，请等待进入战斗画面后再点");
        else
            SetFeedback(FB_WARN, "操作已拦截：本场战斗已进入结算阶段，禁止再写入状态（请等待进入下一关）");
        InterlockedIncrement(&g_reqDone);
        return;
    }

    switch (kind)
    {
    case REQ_DUMP_CLASS:
        Log("[user] dump 请求：%s", g_req.text);
        ReflectDumpClass(g_req.text, nullptr, 200);
        SetFeedback(FB_OK, "已导出类 \"%s\" 结构至日志 (run/mod_runtime/il2cpp_mod.log)", g_req.text);
        needRefreshSnap = false;
        break;

    case REQ_SCAN_ROSTER:
        PollBattleSessionGuard();
        RefreshBattleSnapshot();
        SetFeedback(g_battlePhase == PHASE_PLAYING ? FB_OK : FB_WARN,
                    g_battlePhase == PHASE_PLAYING
                        ? "已刷新本局实时快照：带入干员 %d 名 | 场上存活怪物 %d 只 (%d 种)"
                        : "当前未处于战斗进行中（阶段=%ld），进入关卡后会自动加载编队与怪物",
                    g_stageCharCount, g_liveEnemyTotal, g_liveEnemyCount, (long)g_battlePhase);
        needRefreshSnap = false;
        break;

    case REQ_RESET_SESSION:
    {
        ResetPerBattleState(true);
        if (curPhase == PHASE_PLAYING)
        {
            if (Il2CppObject *bc = GetBattleControllerFast())
            {
                if (InterlockedCompareExchange(&g_mainSpeedBusy, 1, 0) == 0)
                {
                    g_mainSpeedCtx.bcHandle   = ArkGcPin(bc);
                    g_mainSpeedCtx.mode       = 0;
                    g_mainSpeedCtx.speedLevel = 1; // 恢复 1x 标准速度
                    g_mainSpeedCtx.timeScale  = 1.0f;
                    if (ArkTaskPostMain(MainThreadBattleSpeedTask, &g_mainSpeedCtx, "battle.speed.reset"))
                    {
                        if (HWND hwnd = UiGameHwnd())
                            PostMessageW(hwnd, WM_APP + 0x4D4F, 0, 0);
                    }
                    else
                    {
                        ArkGcUnpin(g_mainSpeedCtx.bcHandle);
                        InterlockedExchange(&g_mainSpeedBusy, 0);
                    }
                }
            }
            RefreshBattleSnapshot();
        }
        SetFeedback(FB_OK, "已一键复位本局状态：关闭全部 4 项持续锁定 + 重置为全体干员 + 恢复标准速度 1x");
        needRefreshSnap = false;
        break;
    }

    case REQ_MOD_CARD_COST:
    {
        int cnt = ApplyCardCostDelta(g_req.text, (int32_t)g_req.val, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "已将 %d 张手牌 (%s) 设为 0 费部署并立即刷新卡槽 UI",
                        cnt, matched[0] ? matched : "匹配干员");
        else if (!g_inBattle)
            SetFeedback(FB_WARN, "修改未生效：当前不在战斗关卡中，请先进入战斗");
        else
            SetFeedback(FB_WARN, "修改未生效：本局手牌中未找到匹配 \"%s\" 的干员，请直接点选上方编队干员按钮", g_req.text);
        break;
    }

    case REQ_MOD_CARD_RESPAWN:
    {
        int cnt = ApplyCardResetRespawn(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "已秒清 %d 张手牌 (%s) 的再部署冷却并设为 1s 重下",
                        cnt, matched[0] ? matched : "匹配干员");
        else
            SetFeedback(FB_WARN, "未找到匹配 \"%s\" 的手牌（请确认已在战斗中并点选本局干员）", g_req.text);
        break;
    }

    case REQ_MOD_DEPLOY_LIMIT:
    {
        int cnt = ApplyFreeDeployLimit(g_req.text);
        if (cnt > 0 || g_inBattle)
            SetFeedback(FB_OK, "已解除 %d 张手牌的部署人数占用，并将战场部署上限 +99", cnt);
        else
            SetFeedback(FB_WARN, "未生效：当前未在战斗关卡中");
        break;
    }

    case REQ_MOD_CHAR_ATTR:
    {
        float beforeVal = 0.0f;
        int cnt = ApplyCharacterAttribute(g_req.text, g_req.attrType, g_req.val, matched, sizeof(matched), &beforeVal);
        const char *attrName =
            (g_req.attrType == 7)  ? "攻击速度" :
            (g_req.attrType == 8)  ? "基础攻击间隔" :
            (g_req.attrType == 1)  ? "攻击力" :
            (g_req.attrType == 0)  ? "最大生命" :
            (g_req.attrType == 2)  ? "防御力" :
            (g_req.attrType == 3)  ? "法术抗性" :
            (g_req.attrType == 5)  ? "阻挡数" :
            (g_req.attrType == 14) ? "每秒技力回复" : "属性";
        if (cnt > 0)
            SetFeedback(FB_OK, "已修改场上 %d 名干员 (%s) %s：%.2f -> %.2f",
                        cnt, matched[0] ? matched : "匹配干员", attrName, beforeVal, g_req.val);
        else if (!g_inBattle)
            SetFeedback(FB_WARN, "修改未生效：当前不在战斗关卡中");
        else
            SetFeedback(FB_WARN, "场上暂无已部署的 \"%s\"（已提前写入手牌快照；请先将干员部署上场再修改实时属性）",
                        g_req.text[0] ? g_req.text : "我方干员");
        break;
    }

    case REQ_MOD_CHAR_FULL_HPSP:
    {
        int cnt = ApplyCharacterFullHpSp(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "已为场上 %d 名干员 (%s) 瞬间回满生命值与技力 (SP)",
                        cnt, matched[0] ? matched : "全体");
        else
            SetFeedback(FB_WARN, "场上未找到已部署的匹配干员（请先将干员部署上场）");
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
            SetFeedback(FB_OK, "弹药修改成功：%s 弹药 %d/%d -> %d/%d（共更新 %d 个弹药计数器）",
                        matched[0] ? matched : "匹配干员", bR, bM, aR, aM, cnt);
        else if (!g_inBattle)
            SetFeedback(FB_WARN, "弹药修改未生效：当前未在战斗关卡中");
        else if (matchedChars == 0)
            SetFeedback(FB_WARN, "弹药修改未生效：场上暂无已部署的匹配干员（请先部署上场并开启/就绪技能）");
        else
            SetFeedback(FB_WARN, "已检测到场上 %d 名匹配干员，但当前均无激活的弹药计数器（仅弹药类技能如水陈/维什戴尔/灰烬/提丰等拥有弹药条；技能开启瞬间生效）",
                        matchedChars);
        break;
    }

    case REQ_MOD_ANGEL_PRESET:
    {
        int c1 = ApplyCardCostDelta(g_req.text, -99, matched, sizeof(matched));
        ApplyCardResetRespawn(g_req.text, matched, sizeof(matched));
        ApplyFreeDeployLimit(g_req.text);
        ApplyBattleDp(99);
        int c2 = ApplyCharacterAttribute(g_req.text, 7,  g_charAtkSpeed,    matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          8,  g_charBaseAtkTime, matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          1,  g_charAtk,         matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          0,  g_charMaxHp,       matched, sizeof(matched));
        ApplyCharacterAttribute(g_req.text,          14, g_charSpRecov,     matched, sizeof(matched));
        ApplyCharacterFullHpSp(g_req.text, matched, sizeof(matched));
        int ammoDummy = 0;
        int c3 = ApplyCharacterAmmo(g_req.text, g_charAmmoTarget, true, &ammoDummy, matched, sizeof(matched),
                                    nullptr, nullptr, nullptr, nullptr);
        if (c1 > 0 || c2 > 0)
            SetFeedback(FB_OK, "超载机枪模式已生效：手牌0费/0冷却=%d张 | 场上属性=%d名(攻速%.0f/间隔%.2fs/攻击%.0f) | 弹药=%d处 | DP=99",
                        c1, c2, g_charAtkSpeed, g_charBaseAtkTime, g_charAtk, c3);
        else
            SetFeedback(FB_WARN, "超载未命中目标：请确认已进入战斗关卡且目标干员已带入本局");
        break;
    }

    case REQ_MOD_BATTLE_DP:
        if (ApplyBattleDp((int32_t)g_req.val))
            SetFeedback(FB_OK, "已将战场当前总费用 (DP) 设为 %d（上限 999）", (int)g_req.val);
        else
            SetFeedback(FB_WARN, "未找到 BattleController（请确认已进入战斗关卡）");
        break;

    case REQ_MOD_BATTLE_LIFE:
        if (ApplyBattleLifePoint((int32_t)g_req.val))
            SetFeedback(FB_OK, "已将战场蓝门生命值设为 %d", (int)g_req.val);
        else
            SetFeedback(FB_WARN, "未找到 BattleController（请确认已进入战斗关卡）");
        break;

    case REQ_MOD_BATTLE_SPEED:
    {
        if (InterlockedCompareExchange(&g_mainSpeedBusy, 1, 0) != 0)
        {
            SetFeedback(FB_WARN, "上一次调速任务仍在主线程排队中");
            break;
        }
        const ArkClassEntry *bcCe = ArkReflectFindClass("Torappu.Battle.BattleController");
        Il2CppObject *bcs[8] = { nullptr };
        int bcn = (bcCe && bcCe->klass) ? ArkGcEnumObjects(bcCe->klass, bcs, 8, 1) : 0;
        if (bcn <= 0 || !bcs[0])
        {
            InterlockedExchange(&g_mainSpeedBusy, 0);
            SetFeedback(FB_WARN, "调速未生效：当前不在战斗关卡中");
            break;
        }
        g_mainSpeedCtx.bcHandle   = ArkGcPin(bcs[0]);
        g_mainSpeedCtx.mode       = g_req.extraInt1;
        g_mainSpeedCtx.speedLevel = g_req.extraInt2;
        g_mainSpeedCtx.timeScale  = g_req.val;
        if (ArkTaskPostMain(MainThreadBattleSpeedTask, &g_mainSpeedCtx, "battle.speed"))
        {
            if (HWND hwnd = UiGameHwnd())
                PostMessageW(hwnd, WM_APP + 0x4D4F, 0, 0);
        }
        else
        {
            ArkGcUnpin(g_mainSpeedCtx.bcHandle);
            InterlockedExchange(&g_mainSpeedBusy, 0);
            SetFeedback(FB_ERR, "调速失败：主线程队列已满");
        }
        break;
    }

    case REQ_MOD_ENEMY_ATTR:
    {
        float beforeVal = 0.0f;
        int cnt = ApplyEnemyAttribute(g_req.text, g_req.attrType, g_req.val, matched, sizeof(matched), &beforeVal);
        const char *attrName =
            (g_req.attrType == 0) ? "最大生命" :
            (g_req.attrType == 1) ? "攻击力" :
            (g_req.attrType == 2) ? "防御力" :
            (g_req.attrType == 3) ? "法术抗性" :
            (g_req.attrType == 6) ? "移动速度" :
            (g_req.attrType == 7) ? "攻击速度" : "属性";
        if (cnt > 0)
            SetFeedback(FB_OK, "已修改场上 %d 只怪物 (%s) %s：%.2f -> %.2f",
                        cnt, matched[0] ? matched : "全部敌人", attrName, beforeVal, g_req.val);
        else
            SetFeedback(FB_WARN, "场上当前没有匹配 \"%s\" 的存活怪物（等敌人出红门后再操作）",
                        g_req.text[0] ? g_req.text : "全部敌人");
        break;
    }

    case REQ_MOD_ENEMY_SANDBAG:
    {
        int cnt = ApplyEnemyAttribute(g_req.text, 0, 1000000.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 1, 0.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 2, 0.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 3, 0.0f, matched, sizeof(matched));
        ApplyEnemyAttribute(g_req.text, 6, 0.0f, matched, sizeof(matched));
        ApplyEnemyUnharmful(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "已将场上 %d 只怪物 (%s) 改造成 100万血/0攻/0防/定身沙包（进蓝门不扣血）",
                        cnt, matched[0] ? matched : "全部敌人");
        else
            SetFeedback(FB_WARN, "场上当前没有匹配存活怪物，无法改造沙包");
        break;
    }

    case REQ_MOD_ENEMY_UNHARMFUL:
    {
        int cnt = ApplyEnemyUnharmful(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "已将场上 %d 只怪物 (%s) 设为进蓝门不扣血", cnt, matched[0] ? matched : "全部敌人");
        else
            SetFeedback(FB_WARN, "场上当前没有匹配存活怪物");
        break;
    }

    case REQ_MOD_ENEMY_KILL:
    {
        int cnt = ApplyEnemyKill(g_req.text, matched, sizeof(matched));
        if (cnt > 0)
            SetFeedback(FB_OK, "已秒杀场上 %d 只匹配怪物 (%s)", cnt, matched[0] ? matched : "全部敌人");
        else
            SetFeedback(FB_WARN, "场上当前没有匹配存活怪物");
        break;
    }

    case REQ_SCAN_STAGE_ENEMIES:
    {
        int cnt = ScanStageEnemies();
        if (cnt > 0)
            SetFeedback(FB_OK, "已读取本关预加载怪物：%d 种（共 %ld 条路线）", cnt, (long)g_stageRouteCount);
        else
            SetFeedback(FB_WARN, "未读取到本关怪物（请先进入战斗关卡）");
        needRefreshSnap = false;
        break;
    }

    case REQ_SPAWN_ENEMY:
    {
        if (InterlockedCompareExchange(&g_mainSpawnBusy, 1, 0) != 0)
        {
            SetFeedback(FB_WARN, "上一次刷怪任务仍在主线程排队执行");
            break;
        }
        if (g_stageEnemyCount <= 0)
            ScanStageEnemies();

        const char *keyToUse = g_req.text;
        if ((!keyToUse || !keyToUse[0]) && g_stageEnemyCount > 0)
        {
            keyToUse = g_stageEnemies[0].id;
            CopyStrSafe(g_spawnEnemyKey, sizeof(g_spawnEnemyKey), keyToUse);
        }

        const ArkClassEntry *bcCe = ArkReflectFindClass("Torappu.Battle.BattleController");
        Il2CppObject *bcs[8] = { nullptr };
        int bcn = (bcCe && bcCe->klass) ? ArkGcEnumObjects(bcCe->klass, bcs, 8, 1) : 0;
        if (bcn <= 0 || !bcs[0] || !keyToUse || !keyToUse[0])
        {
            InterlockedExchange(&g_mainSpawnBusy, 0);
            SetFeedback(FB_WARN, "刷怪未生效：未在战斗关卡中或未选定本关怪物");
            break;
        }

        g_mainSpawnCtx.bcHandle    = ArkGcPin(bcs[0]);
        CopyStrSafe(g_mainSpawnCtx.enemyKey, sizeof(g_mainSpawnCtx.enemyKey), keyToUse);
        g_mainSpawnCtx.routeIdx    = g_req.extraInt1;
        g_mainSpawnCtx.count       = g_req.extraInt2;
        g_mainSpawnCtx.applyCustom = g_spawnApplyCustom ? 1 : 0;
        g_mainSpawnCtx.maxHp       = g_enemyMaxHp;
        g_mainSpawnCtx.atk         = g_enemyAtk;
        g_mainSpawnCtx.def         = g_enemyDef;
        g_mainSpawnCtx.magicRes    = g_enemyMagicRes;
        g_mainSpawnCtx.moveSpd     = g_enemyMoveSpd;

        if (ArkTaskPostMain(MainThreadSpawnEnemyTask, &g_mainSpawnCtx, "enemy.spawn"))
        {
            if (HWND hwnd = UiGameHwnd())
                PostMessageW(hwnd, WM_APP + 0x4D4F, 0, 0);
            SetFeedback(FB_INFO, "已投递刷怪请求 (%s ×%d) 至 Unity 主线程...", keyToUse, g_req.extraInt2);
        }
        else
        {
            ArkGcUnpin(g_mainSpawnCtx.bcHandle);
            InterlockedExchange(&g_mainSpawnBusy, 0);
            SetFeedback(FB_ERR, "刷怪失败：主线程任务队列已满");
        }
        break;
    }

    default:
        needRefreshSnap = false;
        break;
    }

    if (needRefreshSnap)
        RefreshBattleSnapshot();

    InterlockedIncrement(&g_reqDone);
}

// ================================================================ 扩展点实现

void Mod_OnReady()
{
    Log("[user] Mod_OnReady —— 初始化内置运行时分析工具箱 + 战斗定制工作台");

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

// Worker 线程，每 100ms 一次（已 attach 域）
void Mod_OnTick()
{
    // ① 零 GC 停顿检测对战生命周期与脏状态（换关/重开/结算/代理指挥自动守卫）
    PollBattleSessionGuard();

    ConsumeRequest();
    ArkTaskPump();
    ArkWatchApplyWrites();

    const bool playingNow = (InterlockedCompareExchange(&g_battlePhase, 0, 0) == PHASE_PLAYING);
    ++g_tickCounter;

    // ② 持续自动锁定：仅在真实战斗进行中 (PLAYING) 运行；结算或大厅自动暂停
    if (playingNow)
    {
        if (InterlockedCompareExchange(&g_lockBattleDp, 0, 0) != 0)
            ApplyBattleDp(99);
        if ((g_tickCounter % 2) == 0 &&
            (g_lockInfAmmo || g_lockInfSp || g_lockGodHp))
        {
            PumpPersistentLocks();
        }
    }

    // ③ 仅在面板展开且处于战斗进行中时刷新本局编队/怪物快照（关闭面板 0 次 GC 停顿）
    static bool s_prevPanelVis = false;
    const bool panelVis = UiPanelVisible();
    if (panelVis && playingNow && (!s_prevPanelVis || (g_tickCounter % 15) == 0))
    {
        RefreshBattleSnapshot();
    }
    s_prevPanelVis = panelVis;

    // ④ 每 500ms 刷新一次 Watch 缓存并落盘配置
    if ((g_tickCounter % 5) == 0)
    {
        ArkWatchRefresh();
        if (ArkCfgTakeDirty())
            ArkCfgSave();
    }
}

// 游戏渲染线程，每帧（面板关着也跑）
void Mod_OnFrame()
{
    if (ArkTaskBusMainThread() == 0)
    {
        if (HWND hwnd = UiGameHwnd())
            ArkTaskBusSetMainThread(GetWindowThreadProcessId(hwnd, NULL));
    }
    if (ArkMainTaskPending() > 0)
    {
        if (HWND hwnd = UiGameHwnd())
            PostMessageW(hwnd, WM_APP + 0x4D4F, 0, 0);
    }
    ArkHotkeyPoll();
}

// Unity 主线程（ExitWndProc 收到 WM_APP+0x4D4F）
void Mod_OnGameThread()
{
    ArkWatchApplyWrites();
    if (ArkTaskBusIsMainThread())
    {
        ir::EnsureAttached();
        ArkMainTaskPump();
    }
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

// 渲染线程，在面板 TabBar 里追加全部功能标签页
void Mod_DrawPanelTabs()
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

    if (!ImGui::BeginTabItem("战斗定制(干员/弹药/怪物)"))
        return;

    // 拷贝一份当前快照供本帧渲染使用（零阻塞、线程安全）
    StageCharInfo localChars[32];
    int localCharCount = 0;
    LiveEnemyGroup localEnemies[32];
    int localEnemyCount = 0;
    int localEnemyTotal = 0;
    SpinLock(&g_snapLock);
    localCharCount = g_stageCharCount;
    for (int i = 0; i < localCharCount; ++i) localChars[i] = g_stageChars[i];
    localEnemyCount = g_liveEnemyCount;
    localEnemyTotal = g_liveEnemyTotal;
    for (int i = 0; i < localEnemyCount; ++i) localEnemies[i] = g_liveEnemies[i];
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

    // ========================================================================
    // 顶部对战状态守卫栏 + 醒目即时反馈横幅
    // ========================================================================
    const BattlePhase curPhase = (BattlePhase)InterlockedCompareExchange(&g_battlePhase, 0, 0);
    const bool isAutoReplay = (InterlockedCompareExchange(&g_isAutoReplay, 0, 0) != 0);
    const LONG curSessId = InterlockedCompareExchange(&g_battleSessId, 0, 0);
    const bool battleReady = (curPhase == PHASE_PLAYING);

    {
        ImVec4 stCol;
        char stText[220];
        if (curPhase == PHASE_PLAYING && isAutoReplay)
        {
            stCol = ImVec4(1.00f, 0.58f, 0.18f, 1.0f);
            _snprintf_s(stText, sizeof(stText), _TRUNCATE,
                        "⚠ 代理指挥录像中 [第 #%ld 局 · %s] — 已自动暂停持续锁定以防代理轴错乱翻车",
                        (long)curSessId, g_curLevelId[0] ? g_curLevelId : "战斗关卡");
        }
        else if (curPhase == PHASE_PLAYING)
        {
            stCol = ImVec4(0.30f, 0.95f, 0.55f, 1.0f);
            _snprintf_s(stText, sizeof(stText), _TRUNCATE,
                        "● 战斗进行中 [第 #%ld 局 · %s] — 全部定制功能已解锁（切关/重开会自动防脏重置）",
                        (long)curSessId, g_curLevelId[0] ? g_curLevelId : "战斗关卡");
        }
        else if (curPhase == PHASE_LOADING)
        {
            stCol = ImVec4(1.00f, 0.85f, 0.30f, 1.0f);
            CopyStrSafe(stText, sizeof(stText), "○ 关卡初始化加载中... — 请稍候 (<1秒) 进入战斗画面后自动解锁");
        }
        else if (curPhase == PHASE_FINISHED)
        {
            stCol = ImVec4(0.95f, 0.72f, 0.32f, 1.0f);
            CopyStrSafe(stText, sizeof(stText), "○ 本场战斗已结算 — 持续锁定已暂停、修改已锁定，进入下一关后自动重置");
        }
        else
        {
            stCol = ImVec4(0.55f, 0.80f, 1.00f, 1.0f);
            CopyStrSafe(stText, sizeof(stText), "○ 当前位于主界面/大厅 — Mod 已注入就绪！进入任意战斗关卡后本页功能自动解锁");
        }

        ImVec4 stBg = ImVec4(stCol.x * 0.14f, stCol.y * 0.14f, stCol.z * 0.14f, 0.88f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, stBg);
        ImGui::PushStyleColor(ImGuiCol_Border, stCol);
        if (ImGui::BeginChild("##PhaseBanner", ImVec2(0.0f, 34.0f), true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::TextColored(stCol, "%s", stText);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    {
        ImVec4 col = FeedbackColor(curFb.level);
        ImVec4 bg  = ImVec4(col.x * 0.14f, col.y * 0.14f, col.z * 0.14f, 0.82f);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
        ImGui::PushStyleColor(ImGuiCol_Border, col);
        if (ImGui::BeginChild("##FeedbackBanner", ImVec2(0.0f, 34.0f), true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::TextColored(col, "%s %s  %s",
                               FeedbackPrefix(curFb.level),
                               curFb.timeStr[0] ? curFb.timeStr : "--:--:--",
                               curFb.msg);
            ImGui::SameLine(ImGui::GetWindowWidth() - 150.0f);
            ImGui::TextDisabled("请求 %ld/%ld", (long)g_reqDone, (long)g_reqSeq);
        }
        ImGui::EndChild();
        ImGui::PopStyleColor(2);
    }

    // 快捷控制栏：刷新快照 + 一键复位本局 + 防脏状态自动重置开关
    if (ImGui::SmallButton("刷新快照"))
        PostRequestEx(REQ_SCAN_ROSTER, "", 0, 0.0f);
    ImGui::SameLine();
    if (ImGui::SmallButton("一键复位本局 (关全锁+1x速)"))
        PostRequestEx(REQ_RESET_SESSION, "", 0, 0.0f);
    ImGui::SameLine();
    {
        bool autoRst = (InterlockedCompareExchange(&g_autoResetNew, 0, 0) != 0);
        if (ImGui::Checkbox("切关/重开/代理指挥自动重置锁定 (防脏状态)", &autoRst))
        {
            InterlockedExchange(&g_autoResetNew, autoRst ? 1 : 0);
            SetFeedback(FB_INFO, autoRst
                        ? "已开启防脏状态保护：切换关卡/重开/结算/代理指挥时将自动清空锁定与旧目标"
                        : "已关闭切关自动清锁（仅代理指挥仍会保护；跨关刷图时锁定将保持生效）");
        }
    }

    // 非战斗状态时展示清晰的「使用时机指引卡」，消除玩家的时机困惑
    if (!battleReady)
    {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.11f, 0.14f, 0.19f, 0.92f));
        if (ImGui::BeginChild("##TimingGuide", ImVec2(0.0f, 68.0f), true, ImGuiWindowFlags_NoScrollbar))
        {
            ImGui::TextColored(ImVec4(0.65f, 0.90f, 1.0f, 1.0f), "何时可以使用 Mod？（防误操作保护已生效，下方战斗按钮已自动置灰）");
            ImGui::TextDisabled("  1. 游戏启动后随时可按 [INSERT] 开关面板；主界面仅可使用「类浏览器 / 对象检查 / Dumper」等分析工具。");
            ImGui::TextDisabled("  2. 进入任意关卡并出现战场画面后，本页立即解锁并自动列出本局编队干员；手牌0费/DP/倍速进关即改，攻速/攻击/弹药需干员上场后改。");
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    ImGui::Separator();

    ImGui::BeginDisabled(!battleReady);
    if (ImGui::BeginTabBar("##BattleCustomSubTabs"))
    {
        // ============================================================
        // 子页 1：我方干员定制 & 弹药 & 持续锁定
        // ============================================================
        if (ImGui::BeginTabItem("① 我方干员 & 弹药定制"))
        {
            // ---- A. 零误输：本局编队干员一键点选栏 + 中文模糊搜索联想 ----
            ImGui::TextColored(ImVec4(0.55f, 0.90f, 1.0f, 1.0f),
                               "A. 选择修改对象（已自动限制并列出本局带入的全部干员，点击即选中，杜绝手打错别字）");

            ImGui::SetNextItemWidth(220.0f);
            bool searchEnter = ImGui::InputTextWithHint("##charSearch", "搜索本局干员（支持中文名/ID，回车选中）",
                                                        g_charSearch, sizeof(g_charSearch),
                                                        ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SameLine();
            if (ImGui::SmallButton("清空搜索"))
                g_charSearch[0] = 0;
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.90f, 0.45f, 1.0f), "当前已锁定目标：%s", g_selectedCharLabel);
            if (g_selectedCharId[0])
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("切回全体干员"))
                {
                    g_selectedCharId[0] = 0;
                    CopyStrSafe(g_selectedCharLabel, sizeof(g_selectedCharLabel), "全体编队干员");
                }
            }

            // 渲染本局编队干员芯片按钮
            {
                bool allSel = (g_selectedCharId[0] == 0);
                if (allSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.35f, 1.0f));
                char allBtn[64];
                _snprintf_s(allBtn, sizeof(allBtn), _TRUNCATE, "★ 全体编队干员 (%d人)###chr_all", localCharCount);
                if (ImGui::SmallButton(allBtn))
                {
                    g_selectedCharId[0] = 0;
                    CopyStrSafe(g_selectedCharLabel, sizeof(g_selectedCharLabel), "全体编队干员");
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
                    if (isSel)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.52f, 0.12f, 1.0f));
                    else if (c.deployed)
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.16f, 0.42f, 0.55f, 1.0f));
                    else
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.24f, 0.26f, 0.32f, 1.0f));

                    char chip[180];
                    if (c.deployed && c.hasAmmo)
                        _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s [场上 弹%d/%d]###c%d",
                                    c.nameCn[0] ? c.nameCn : c.id, c.ammoRem, c.ammoMax, i);
                    else if (c.deployed)
                        _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s [场上]###c%d",
                                    c.nameCn[0] ? c.nameCn : c.id, i);
                    else
                        _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s [手牌 %d费]###c%d",
                                    c.nameCn[0] ? c.nameCn : c.id, c.cost, i);

                    if (ImGui::SmallButton(chip))
                    {
                        CopyStrSafe(g_selectedCharId, sizeof(g_selectedCharId), c.id);
                        _snprintf_s(g_selectedCharLabel, sizeof(g_selectedCharLabel), _TRUNCATE,
                                    "%s (%s)", c.nameCn[0] ? c.nameCn : c.id, c.id);
                    }
                    ImGui::PopStyleColor();
                }

                // 在搜索框按回车时自动锁定首个匹配的本局干员
                if (searchEnter && firstMatchIdx >= 0)
                {
                    const StageCharInfo &m = localChars[firstMatchIdx];
                    CopyStrSafe(g_selectedCharId, sizeof(g_selectedCharId), m.id);
                    _snprintf_s(g_selectedCharLabel, sizeof(g_selectedCharLabel), _TRUNCATE,
                                "%s (%s)", m.nameCn[0] ? m.nameCn : m.id, m.id);
                    SetFeedback(FB_OK, "已自动选中本局干员：%s", g_selectedCharLabel);
                }

                if (localCharCount == 0)
                {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f),
                                       "（进入任意战斗关卡后会自动显示本局编队干员）");
                }
                else if (g_charSearch[0] && shown == 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.78f, 0.30f, 1.0f),
                                       "⚠ 本局编队中未找到包含 \"%s\" 的干员（可能未带入或有错别字）—— 请清空搜索后直接点选本局干员",
                                       g_charSearch);
                }
            }

            // ---- 选中干员的实时属性回显条（改完立刻肉眼可见数值变化）----
            if (g_selectedCharId[0])
            {
                const StageCharInfo *sel = nullptr;
                for (int i = 0; i < localCharCount; ++i)
                {
                    if (strcmp(localChars[i].id, g_selectedCharId) == 0) { sel = &localChars[i]; break; }
                }
                if (sel)
                {
                    ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.10f, 0.14f, 0.18f, 0.90f));
                    if (ImGui::BeginChild("##CharTelemetry", ImVec2(0.0f, 50.0f), true, ImGuiWindowFlags_NoScrollbar))
                    {
                        ImGui::TextColored(ImVec4(0.45f, 0.95f, 0.70f, 1.0f),
                                           "实时数值 | %s [%s]  费用:%d  生命:%.0f/%.0f  攻击:%.0f  攻速:%.0f  间隔:%.2fs",
                                           sel->nameCn[0] ? sel->nameCn : sel->id,
                                           sel->deployed ? "已部署场上" : "手牌待部署",
                                           sel->cost, sel->hp, sel->maxHp, sel->atk, sel->atkSpd, sel->baseAtkTime);
                        if (sel->hasAmmo)
                            ImGui::TextColored(ImVec4(1.0f, 0.88f, 0.35f, 1.0f),
                                               "         | 防御:%.0f  法抗:%.0f  阻挡:%.0f  技力:%.1f/%.0f  ★弹药: %d / %d",
                                               sel->def, sel->magicRes, sel->blockCnt, sel->sp, sel->maxSp,
                                               sel->ammoRem, sel->ammoMax);
                        else
                            ImGui::TextDisabled("         | 防御:%.0f  法抗:%.0f  阻挡:%.0f  技力:%.1f/%.0f  弹药: 当前无激活弹药条",
                                                sel->def, sel->magicRes, sel->blockCnt, sel->sp, sel->maxSp);
                    }
                    ImGui::EndChild();
                    ImGui::PopStyleColor();
                }
                else if (localCharCount > 0)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.30f, 1.0f),
                                       "⚠ 当前锁定干员 (%s) 未带入本局，请点选上方本局编队干员或切回全体", g_selectedCharLabel);
                }
            }

            // ---- B. 持续自动锁定开关（每 100ms 维持）----
            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.95f, 0.65f, 1.0f, 1.0f), "B. 持续自动锁定（勾选后每 0.1s 自动维持，作用于当前选中干员/全体）");
            {
                bool bAmmo = (g_lockInfAmmo != 0);
                if (ImGui::Checkbox("无限弹药锁定", &bAmmo))
                {
                    InterlockedExchange(&g_lockInfAmmo, bAmmo ? 1 : 0);
                    SetFeedback(FB_OK, "已%s「无限弹药锁定」（目标：%s）", bAmmo ? "开启" : "关闭", g_selectedCharLabel);
                }
                ImGui::SameLine();
                bool bSp = (g_lockInfSp != 0);
                if (ImGui::Checkbox("无限技力锁定(永续满SP)", &bSp))
                {
                    InterlockedExchange(&g_lockInfSp, bSp ? 1 : 0);
                    SetFeedback(FB_OK, "已%s「无限技力锁定」（目标：%s）", bSp ? "开启" : "关闭", g_selectedCharLabel);
                }
                ImGui::SameLine();
                bool bHp = (g_lockGodHp != 0);
                if (ImGui::Checkbox("干员锁血无敌", &bHp))
                {
                    InterlockedExchange(&g_lockGodHp, bHp ? 1 : 0);
                    SetFeedback(FB_OK, "已%s「干员锁血无敌」（目标：%s）", bHp ? "开启" : "关闭", g_selectedCharLabel);
                }
                ImGui::SameLine();
                bool bDp = (g_lockBattleDp != 0);
                if (ImGui::Checkbox("战场 DP 锁定 99", &bDp))
                {
                    InterlockedExchange(&g_lockBattleDp, bDp ? 1 : 0);
                    SetFeedback(FB_OK, "已%s「战场 DP 锁定 99」", bDp ? "开启" : "关闭");
                }
            }

            // ---- C. 一键超载 + 手牌与部署 + 弹药直改 ----
            ImGui::Separator();
            if (ImGui::Button("★ 一键开启超载机枪模式 (0费/0秒重下/不占人口/99DP + 500攻速/0.12s间隔/5000攻/满血满SP/满弹药)", ImVec2(-1.0f, 28.0f)))
                PostRequestEx(REQ_MOD_ANGEL_PRESET, g_selectedCharId, 0, 0.0f);

            if (ImGui::Button("手牌设为 0 费部署"))
                PostRequestEx(REQ_MOD_CARD_COST, g_selectedCharId, 4, (float)g_cardCostDelta);
            ImGui::SameLine();
            if (ImGui::Button("秒清再部署冷却 (0秒重下)"))
                PostRequestEx(REQ_MOD_CARD_RESPAWN, g_selectedCharId, 21, 1.0f);
            ImGui::SameLine();
            if (ImGui::Button("解除部署人数上限 (+99)"))
                PostRequestEx(REQ_MOD_DEPLOY_LIMIT, g_selectedCharId, 0, 0.0f);
            ImGui::SameLine();
            if (ImGui::Button("场上干员满血 + 满技力"))
                PostRequestEx(REQ_MOD_CHAR_FULL_HPSP, g_selectedCharId, 0, 0.0f);

            // 弹药/子弹直改行
            ImGui::SetNextItemWidth(110.0f);
            ImGui::InputInt("目标弹药数", &g_charAmmoTarget, 10, 50);
            if (g_charAmmoTarget < 1) g_charAmmoTarget = 1;
            if (g_charAmmoTarget > 9999) g_charAmmoTarget = 9999;
            ImGui::SameLine();
            if (ImGui::Button("修改当前弹药 & 弹药上限"))
                PostRequestEx(REQ_MOD_CHAR_AMMO, g_selectedCharId, 0, (float)g_charAmmoTarget, 1, 0);
            ImGui::SameLine();
            if (ImGui::Button("仅补满当前弹药 (保持原上限)"))
                PostRequestEx(REQ_MOD_CHAR_AMMO, g_selectedCharId, 0, (float)g_charAmmoTarget, 0, 0);

            // ---- D. 场上干员实时属性定制 ----
            ImGui::Separator();
            ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.45f, 1.0f), "C. 场上干员实时属性精细定制（自动突破 600 攻速上限；请先部署上场）");

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("攻击速度##cs", &g_charAtkSpeed, 50.0f, 100.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改攻速"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 7, g_charAtkSpeed);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("攻击间隔(s)##cb", &g_charBaseAtkTime, 0.05f, 0.1f, "%.2f");
            ImGui::SameLine();
            if (ImGui::Button("改攻击间隔"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 8, g_charBaseAtkTime);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("攻击力##ca", &g_charAtk, 500.0f, 1000.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改攻击力"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 1, g_charAtk);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("最大生命##ch", &g_charMaxHp, 5000.0f, 10000.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改生命并回满"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 0, g_charMaxHp);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("防御力##cd", &g_charDef, 200.0f, 500.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改防御力"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 2, g_charDef);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("法术抗性##cm", &g_charMagicRes, 10.0f, 20.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改法术抗性"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 3, g_charMagicRes);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("阻挡数##ck", &g_charBlockCnt, 1.0f, 2.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改阻挡数"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 5, g_charBlockCnt);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("每秒技力##cp", &g_charSpRecov, 5.0f, 10.0f, "%.1f");
            ImGui::SameLine();
            if (ImGui::Button("改技力回复并满SP"))
                PostRequestEx(REQ_MOD_CHAR_ATTR, g_selectedCharId, 14, g_charSpRecov);

            ImGui::EndTabItem();
        }

        // ============================================================
        // 子页 2：敌方怪物定制 & 动态刷怪器
        // ============================================================
        if (ImGui::BeginTabItem("② 敌方怪物定制 & 动态刷怪"))
        {
            ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.45f, 1.0f),
                               "A. 场上存活怪物实时改造（点击下方存活怪物按钮一键选定，零手打错误）");

            // 场上存活怪物芯片按钮
            {
                bool allE = (g_enemyFilter[0] == 0);
                if (allE) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.65f, 0.28f, 0.18f, 1.0f));
                char allBtn[64];
                _snprintf_s(allBtn, sizeof(allBtn), _TRUNCATE, "★ 全场所有怪物 (%d只)###enm_all", localEnemyTotal);
                if (ImGui::SmallButton(allBtn))
                {
                    g_enemyFilter[0] = 0;
                    CopyStrSafe(g_enemyLabel, sizeof(g_enemyLabel), "全场所有怪物");
                }
                if (allE) ImGui::PopStyleColor();

                for (int i = 0; i < localEnemyCount; ++i)
                {
                    const LiveEnemyGroup &e = localEnemies[i];
                    if ((i + 1) % 4 != 0) ImGui::SameLine();
                    bool isSel = (g_enemyFilter[0] && strcmp(g_enemyFilter, e.key) == 0);
                    if (isSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.80f, 0.48f, 0.12f, 1.0f));
                    char chip[180];
                    _snprintf_s(chip, sizeof(chip), _TRUNCATE, "%s ×%d (HP:%.0f)###le%d",
                                e.name[0] ? e.name : e.key, e.count, e.maxHp, i);
                    if (ImGui::SmallButton(chip))
                    {
                        CopyStrSafe(g_enemyFilter, sizeof(g_enemyFilter), e.key);
                        _snprintf_s(g_enemyLabel, sizeof(g_enemyLabel), _TRUNCATE,
                                    "%s (%s)", e.name[0] ? e.name : e.key, e.key);
                    }
                    if (isSel) ImGui::PopStyleColor();
                }
            }

            ImGui::SetNextItemWidth(240.0f);
            if (ImGui::InputTextWithHint("怪物过滤(支持中文/ID)", "留空=全场怪物", g_enemyFilter, sizeof(g_enemyFilter)))
                CopyStrSafe(g_enemyLabel, sizeof(g_enemyLabel), g_enemyFilter[0] ? g_enemyFilter : "全场所有怪物");

            if (ImGui::Button("★ 改造成 100万血/0攻/0防/定身木桩沙包 (测DPS专用)"))
                PostRequestEx(REQ_MOD_ENEMY_SANDBAG, g_enemyFilter, 0, 0.0f);
            ImGui::SameLine();
            if (ImGui::Button("定身罚站 (移速=0)"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 6, 0.0f);
            ImGui::SameLine();
            if (ImGui::Button("恢复移速 (1.0)"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 6, 1.0f);
            ImGui::SameLine();
            if (ImGui::Button("进蓝门不扣血"))
                PostRequestEx(REQ_MOD_ENEMY_UNHARMFUL, g_enemyFilter, 0, 0.0f);
            ImGui::SameLine();
            if (ImGui::Button("一键秒杀"))
                PostRequestEx(REQ_MOD_ENEMY_KILL, g_enemyFilter, 0, 0.0f);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("怪物血量##eh", &g_enemyMaxHp, 10000.0f, 100000.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改血量"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 0, g_enemyMaxHp);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("怪物攻击##ea", &g_enemyAtk, 100.0f, 500.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改攻击"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 1, g_enemyAtk);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("怪物防御##ed", &g_enemyDef, 100.0f, 500.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改防御"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 2, g_enemyDef);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("怪物法抗##em", &g_enemyMagicRes, 10.0f, 20.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改法抗"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 3, g_enemyMagicRes);

            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("怪物移速##es", &g_enemyMoveSpd, 0.2f, 0.5f, "%.2f");
            ImGui::SameLine();
            if (ImGui::Button("改移速"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 6, g_enemyMoveSpd);
            ImGui::SameLine(360.0f);
            ImGui::SetNextItemWidth(120.0f);
            ImGui::InputFloat("怪物攻速##ep", &g_enemyAtkSpd, 20.0f, 50.0f, "%.0f");
            ImGui::SameLine();
            if (ImGui::Button("改攻速##eb"))
                PostRequestEx(REQ_MOD_ENEMY_ATTR, g_enemyFilter, 7, g_enemyAtkSpd);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.55f, 0.90f, 1.0f, 1.0f),
                               "B. 本关动态刷怪器（进关自动加载本关怪物库，Unity 主线程安全生成，不卡关卡结算）");
            if (ImGui::SmallButton("重新读取本关可用怪物"))
                PostRequestEx(REQ_SCAN_STAGE_ENEMIES, "", 0, 0.0f);
            ImGui::SameLine();
            ImGui::TextDisabled("本关预加载怪物：%ld 种 | 路线数：%ld 条",
                                (long)g_stageEnemyCount, (long)g_stageRouteCount);

            const int ecnt = (int)g_stageEnemyCount;
            for (int i = 0; i < ecnt; ++i)
            {
                if (i > 0 && (i % 4) != 0) ImGui::SameLine();
                bool isSel = (strcmp(g_spawnEnemyKey, g_stageEnemies[i].id) == 0);
                if (isSel) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.55f, 0.35f, 1.0f));
                char btn[220];
                if (g_stageEnemies[i].name[0])
                    _snprintf_s(btn, sizeof(btn), _TRUNCATE, "%s (%s)###se%d",
                                g_stageEnemies[i].name, g_stageEnemies[i].id, i);
                else
                    _snprintf_s(btn, sizeof(btn), _TRUNCATE, "%s###se%d",
                                g_stageEnemies[i].id, i);
                if (ImGui::SmallButton(btn))
                    CopyStrSafe(g_spawnEnemyKey, sizeof(g_spawnEnemyKey), g_stageEnemies[i].id);
                if (isSel) ImGui::PopStyleColor();
            }

            ImGui::SetNextItemWidth(200.0f);
            ImGui::InputText("刷出怪物 ID", g_spawnEnemyKey, sizeof(g_spawnEnemyKey));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(75.0f);
            ImGui::InputInt("路线", &g_spawnRouteIdx);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(75.0f);
            ImGui::InputInt("数量", &g_spawnCount);
            if (g_spawnCount < 1) g_spawnCount = 1;
            if (g_spawnCount > 20) g_spawnCount = 20;

            ImGui::Checkbox("刷出瞬间套用上方自定义怪物属性", &g_spawnApplyCustom);
            ImGui::SameLine();
            if (ImGui::Button("★ 立即刷出该怪物", ImVec2(190.0f, 0.0f)))
                PostRequestEx(REQ_SPAWN_ENEMY, g_spawnEnemyKey, 0, 0.0f, g_spawnRouteIdx, g_spawnCount);

            ImGui::EndTabItem();
        }

        // ============================================================
        // 子页 3：全局战场控制 & 战斗倍速 & 调试
        // ============================================================
        if (ImGui::BeginTabItem("③ 全局战场 & 倍速控制"))
        {
            ImGui::TextColored(ImVec4(0.6f, 0.9f, 1.0f, 1.0f), "A. 战场全局资源与规则 (Torappu.Battle.BattleController)");
            if (ImGui::Button("当前费用 (DP) = 99 (上限 999)", ImVec2(240.0f, 30.0f)))
                PostRequestEx(REQ_MOD_BATTLE_DP, "", 0, 99.0f);
            ImGui::SameLine();
            if (ImGui::Button("关卡蓝门生命值 = 999", ImVec2(200.0f, 30.0f)))
                PostRequestEx(REQ_MOD_BATTLE_LIFE, "", 0, 999.0f);
            ImGui::SameLine();
            if (ImGui::Button("全体解除部署上限 (+99)", ImVec2(210.0f, 30.0f)))
                PostRequestEx(REQ_MOD_DEPLOY_LIMIT, "", 0, 0.0f);

            ImGui::Separator();
            ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.70f, 1.0f), "B. 战斗倍速控制（Unity 主线程安全调用，不破坏定点数战斗逻辑）");
            if (ImGui::Button("慢速 0.5x (子弹时间)"))
                PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, 0.5f, 0, 0);
            ImGui::SameLine();
            if (ImGui::Button("标准 1x"))
                PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, 1.0f, 0, 1);
            ImGui::SameLine();
            if (ImGui::Button("加速 2x"))
                PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, 2.0f, 0, 2);
            ImGui::SameLine();
            if (ImGui::Button("★ 原生极速 (SUPER_FAST)"))
                PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, 3.0f, 0, 3);

            ImGui::SetNextItemWidth(180.0f);
            ImGui::SliderFloat("自定义时间倍率", &g_customTimeScale, 0.25f, 6.0f, "%.2fx");
            ImGui::SameLine();
            if (ImGui::Button("应用自定义倍速"))
                PostRequestEx(REQ_MOD_BATTLE_SPEED, "", 0, g_customTimeScale, 1, 0);

            ImGui::Separator();
            ImGui::TextDisabled("C. 通用类结构导出到日志文件 (run/mod_runtime/il2cpp_mod.log)");
            static char clsBuf[128] = { 0 };
            ImGui::SetNextItemWidth(320.0f);
            ImGui::InputTextWithHint("##cls", "要 dump 的类（命名空间.类名）", clsBuf, sizeof(clsBuf));
            ImGui::SameLine();
            ImGui::BeginDisabled(!clsBuf[0]);
            if (ImGui::Button("Dump 这个类"))
                PostRequestEx(REQ_DUMP_CLASS, clsBuf, 0, 0.0f);
            ImGui::EndDisabled();

            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
    ImGui::EndDisabled();

    // 底部可折叠「最近操作反馈记录」（保留最新 8 条带时间戳与状态色标）
    ImGui::Separator();
    if (ImGui::CollapsingHeader("最近操作反馈记录 (最新 8 条)", ImGuiTreeNodeFlags_DefaultOpen))
    {
        if (histCount == 0)
        {
            ImGui::TextDisabled("暂无操作记录");
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

    ImGui::EndTabItem();
}

void Mod_OnShutdown()
{
    Log("[user] Mod_OnShutdown —— 保存配置并清理");
    InterlockedExchange(&g_lockInfAmmo, 0);
    InterlockedExchange(&g_lockInfSp, 0);
    InterlockedExchange(&g_lockGodHp, 0);
    InterlockedExchange(&g_lockBattleDp, 0);
    if (ArkCfgTakeDirty())
        ArkCfgSave();
}
