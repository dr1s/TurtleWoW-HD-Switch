/*
 * HDToggle - OctoWoW (WoW 1.12.1 build 5875) HD paketlerini oyun içinden aç/kapat.
 *
 * Kapalı bir paket (Data\patch-X.mpq) arşiv listesinden çıkarılmaz; yalnızca
 * orijinal oyunda da bulunan dosyalarda alttaki (orijinal) sürüm verilir.
 * Sadece o pakette olan dosyalar (paketin kendi DBC'lerinin gösterdiği yeni
 * dokular, modeller) yine paketten okunur, böylece hiçbir şey eksik kalmaz.
 *
 * Kancalar:
 *  - Storm'un arşiv arama çekirdeği (0x6549A0): hArchive == 0 (genel arama)
 *    çağrılarında, dosyanın asıl sahibi kapalı bir paketse istek bir alttaki
 *    açık arşive yönlendirilir.
 *  - LoadScriptFunctions çağrı noktası (0x48FE9C): her arayüz yüklemesinde
 *    (giriş ve /reload) HDPack_* Lua fonksiyonları kaydedilir.
 *
 * Ayarlar DLL'in yanındaki HDToggle.ini dosyasında tutulur (off=BD gibi), böylece
 * oyun açılışından itibaren (giriş ekranı dahil) geçerlidir.
 */
#include <windows.h>
#include <tlhelp32.h>
#include <wininet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#define HDT_VERSION "1.2.7"

/* --- WoW.exe 1.12.1 (5875) adresleri ---------------------------------- */
#define ADDR_STORM_SEARCH    0x006549A0u /* fastcall(arşiv|0, ad, 5 yığın arg), ret 0x14 */
#define ADDR_ARCHIVE_HEAD    0x00C53FE0u /* açık arşiv listesi, önceliğe göre azalan */
#define ADDR_ARCHIVE_LOCK    0x00C54008u /* liste kilidi (CRITICAL_SECTION) */
#define ADDR_LOADSCRIPT_CALL 0x0048FE9Cu /* call LoadScriptFunctions (0x490250) */
#define ADDR_LOADSCRIPT      0x00490250u
#define ADDR_REGISTER_FUNC   0x00704120u /* fastcall(ad, fonk) */

#define ADDR_SFILE_OPEN      0x00656690u /* stdcall SFileOpenFileEx(arşiv, ad, kapsam, &h) */
#define ADDR_SFILE_SIZE      0x006553C0u /* stdcall SFileGetFileSize(h, &yüksek) */
#define ADDR_SFILE_CLOSE     0x00653900u /* stdcall SFileCloseFile(h) */

#define ADDR_LUA_GETTOP      0x006F3070u
#define ADDR_LUA_TONUMBER    0x006F3620u
#define ADDR_LUA_TOSTRING    0x006F3690u
#define ADDR_LUA_PUSHNIL     0x006F37F0u
#define ADDR_LUA_PUSHNUMBER  0x006F3810u
#define ADDR_LUA_PUSHSTRING  0x006F3890u
#define ADDR_LUA_PUSHBOOLEAN 0x006F39F0u

/* Storm arşiv yapısı */
#define ARC_NEXT  0x004 /* sonraki arşiv; alt bit 1 ise liste sonu */
#define ARC_NAME  0x03C /* char[260] tam yol */
#define ARC_PRIO  0x148 /* öncelik */

/* CTexture (Texture.cpp, HTEXTURE, 0x15C bayt) ve global doku listesi */
#define ADDR_TEX_LIST_HEAD   0x00905828u /* ilk doku; alt bit 1 ise liste sonu */
#define ADDR_TEX_LIST_LINK   0x00905820u /* bağlantı ofseti: sonraki = *(doku + ofset + 4) */
#define ADDR_TEX_PROCESS     0x0044A560u /* thiscall(doku): yüklenen BLP'yi işler, GxTex kurar */
#define ADDR_GXTEX_RELEASE   0x00448670u /* fastcall(gxTex): havuza iade eder / yok eder */
#define ADDR_SFILE2_OPEN     0x006477C0u /* stdcall(0, ad, bayrak, &h) - WoW SFile (gevşek dosya + MPQ) */
#define ADDR_SFILE2_SIZE     0x006487F0u /* stdcall(h, &yüksek) */
#define ADDR_SFILE2_READ     0x00648460u /* stdcall(h, tampon, boyut, &okunan, 0, 0) */
#define ADDR_SFILE2_CLOSE    0x00648730u /* stdcall(h) */
#define TEX_NAME   0x00C /* char[260]: SFile'a verilen ad */
#define TEX_FLAGS  0x110 /* bit0: alfa yok, bit1: açma bayrağı */
#define TEX_ASYNC  0x138 /* yükleme nesnesi (+4: dosya içeriği); yüklenirken dolu */
#define TEX_GX     0x140 /* CGxTex* */
#define TEX_GXFLAG 0x150 /* GxTex bayrakları; 0x40 BLP'ye göre eklenir */
#define TEX_BITS   0x120 /* çözülmüş görüntü (bellekte tutulan dokularda) */
#define TEX_FORMAT 0x118
#define TEX_W      0x144
#define TEX_H      0x148
#define ADDR_TEX_DECODE_IMAGE 0x00449840u /* thiscall(doku): yüklenen BLP'yi belleğe çözer */
#define ADDR_TEX_FREE_IMAGE   0x0044B110u /* fastcall(görüntü, biçim, genişlik, yükseklik) */

/* CGxTex (CGxDevice.cpp, 0x54 bayt) ve aygıt */
#define ADDR_GX_DEVICE       0x00C0ED38u /* CGxDevice* */
#define ADDR_GXDEV_TEXSIZE   0x00594CE0u /* thiscall(aygıt, tex, -1, -1): dokunun bellek boyutu */
#define ADDR_GXDEV_TEXDIRTY  0x00594B90u /* thiscall(aygıt, tex, &bölge, hemen): kirli işaretle */
#define GXDEV_TEXMEM   0x088 /* toplam doku belleği sayacı */
#define GXDEV_TEXSTUB  0x3A68 /* yeniden oluşturulacak dokulara konan yer tutucu D3D doku */
#define GXTEX_DIRTY    0x00  /* bayt: güncellenecek */
#define GXTEX_RECREATE 0x01  /* bayt: D3D dokusu yeniden oluşturulacak */
#define GXTEX_FLAGSDIRTY 0x02
#define GXTEX_RECT     0x04  /* {sol, üst, alt, sağ} güncelleme bölgesi */
#define GXTEX_W1A      0x1A  /* iki kelime, -1 */
#define GXTEX_DESC     0x24  /* genişlik, yükseklik, derinlik, hedef, biçim, veri biçimi, bayrak (7 dword) */
#define GXTEX_D3D      0x48  /* IDirect3DBaseTexture9* */
#define GXTEX_USERARG  0x40  /* geri çağırma argümanı (BLP dokularında CTexture*) */
#define GXTEX_USERFUNC 0x44  /* geri çağırma */
#define ADDR_TEX_BLP_CALLBACK 0x00448840u /* UpdateBlpTextureAsync: dosyadan akan BLP dokuları */

/* CM2Cache (M2Cache.cpp): statik global, ad -> CM2Shared hash tablosu.
 * CM2Shared: +0 referans, +4 önbellek, +0x20 ad (uzantısız, 260 bayt), +0x124
 * zincirde kendini gösteren yuvanın adresi, +0x128 sonraki, +0x12C ad hash'i
 * (zincir buna göre sıralı). Arama (0x706A50) dosyayı önce açar, sonra kovada
 * hash'i ve adı karşılaştırır. Release referans sıfıra inince zincirdeki modeli
 * LRU listesine koyar; oyunun temizleyicisi (0x706870) 30 sn sonra bekleyen
 * okumayı iptal edip siler. Zincirde OLMAYAN model ise hemen, okuma iptal
 * edilmeden silinir: okuma bitince silinmiş nesneye yazılır (0x71CD18 çökmesi).
 * Bu yüzden eskiyen model zincirden çıkarılmaz; adının başına hiçbir aramanın
 * eşleşemeyeceği bir işaret konur, gerisi oyunun kendi yaşam döngüsüdür. */
#define ADDR_M2CACHE_BUCKETS 0x00CEEFC4u /* 0xCEEFB0 + 0x14 */
#define M2CACHE_NBUCKETS     1021
#define M2S_NAME 0x020
#define M2S_NAMELEN 0x104
#define M2S_PREV 0x124
#define M2S_NEXT 0x128
#define M2_STALE_MARK '\x01'
/* Önbelleğin LRU listesi (0x706870 temizleyicisinin okuduğu alanlar): referansı sıfıra
 * inen model listenin sonuna eklenir; temizleyici her dünya karesinde (0x707490 ->
 * 0x70749C) baştan, 30 sn'yi doldurmuş olanları siler: LRU'dan çıkar, bekleyen okumayı
 * iptal eder (0x4439B0) ve dosyasını kapatır, yıkıcı (0x71D330) zincirden de çıkarır,
 * SMemFree. Eskimiş modeller hiçbir aramayla yeniden kullanılamaz; 30 sn beklemeleri
 * yalnızca eski ve yeni seçimin modellerini bellekte birlikte tutar (aç/kapat başına
 * yüzlerce MB). Bu yüzden eskimiş olanlar, aynı adımlarla, bir sonraki karede silinir. */
#define M2CACHE_LRU_HEAD     0x0C
#define M2CACHE_LRU_TAILSLOT 0x10
#define M2S_ASYNC    0x0C
#define M2S_LRU_SLOT 0x14 /* listede kendini gösteren yuvanın adresi */
#define M2S_LRU_NEXT 0x18
#define ADDR_M2CACHE_CLEAN       0x00706870u /* thiscall(önbellek) */
#define ADDR_M2CACHE_CLEAN_CALL  0x0070749Cu
#define ADDR_ASYNC_CANCEL        0x004439B0u /* thiscall(okuma) */
#define ADDR_M2SHARED_DTOR       0x0071D330u /* thiscall(model) */
static int g_staleFreed; /* hemen silinen eskimiş modeller (toplam) */
static volatile LONG g_staleUntil; /* son işaretlemeden 60 sn sonrası: o zamana dek her karede bakılır */

/* Nesne yöneticisi ve karakterler (CGUnit_C) */
#define ADDR_OBJMGR        0x00B41414u /* ClntObjMgr*: +0xAC ilk nesne, +0xA4 bağlantı ofseti */
#define OBJMGR_FIRST       0xAC
#define OBJMGR_LINK        0xA4
#define OBJ_DESC           0x08  /* tanım alanları; +8 tip maskesi */
#define TYPEMASK_UNIT      0x08  /* oyuncular da dahil */
#define UNIT_DISPLAYREC    0xB34 /* saklanan CreatureDisplayInfo kaydı */
#define UNIT_COMPONENT     0xD30 /* CCharacterComponent* (yeniden kurmada 0x476000 ile bırakılır) */
#define UNIT_DESC_DISPLAYID 0x1F4 /* tanım alanlarında görünüm kimliği (0x60AE10) */
#define ADDR_WORLDFRAME    0x00B4B2BCu /* CGWorldFrame* */
#define WORLDFRAME_CAMERA  0x65B8      /* kamera (0x4818F0) */
#define CAMERA_TARGET      0x88        /* izlenen nesnenin GUID'i (0x60ACDE) */
#define CDI_RECORD_SIZE    0x30  /* 12 alan (0x542E90 satır boyutu denetimi) */
#define ADDR_UNIT_REBUILD  0x0060ABE0u /* thiscall(unit): görünüm değişince modeli baştan kurar */
#define TYPEMASK_GAMEOBJECT 0x20
#define OBJ_MODEL          0xD8  /* CGObject_C: CM2Model* */
#define OBJ_VT_MODELNAME   (0x28 / 4) /* thiscall(obj, 0) -> model dosya adı */
#define ADDR_OBJ_SETMODEL  0x00613CF0u /* thiscall(obj, model): modeli değiştirir, geri çağırmaları bağlar */

/* Sahne ve modeller (M2Scene.cpp / M2Model) */
#define ADDR_WORLD_SCENE   0x00C7B298u /* CM2Scene*: +8 model listesi başı */
#define ADDR_SCENE_CREATE  0x00707350u /* thiscall(sahne, ad, bayrak) -> CM2Model* (referans 1) */
#define ADDR_MODEL_RELEASE 0x007103A0u /* thiscall(model) */
#define ADDR_MODEL_SETCB   0x007133A0u /* thiscall(model, fonk, arg, x): +0x1F4 geri çağırma */
#define ADDR_MODEL_SETLIGHT 0x007134B0u /* thiscall(model, fonk, arg): +0x3BC ışık geri çağırması */
#define SCENE_MODELS       0x08
#define MODEL_NEXT         0x0C
#define MODEL_SHARED       0x30
#define MODEL_CB           0x1F4
#define MODEL_CBARG        0x1F8

/* Harita dekor objeleri (CMapDoodadDef, WDOODADDEF) */
#define ADDR_DOODAD_CB     0x006951E0u /* dekor modellerine bağlanan geri çağırma; arg = tanım */
#define ADDR_DOODAD_CREATE 0x00695100u /* fastcall(ad, tanım, bayrak=0): modeli oluşturur, konumlar */
#define DOODAD_MODEL       0x88

/* Harita alanları ve bina verisi (Map.cpp / MapObjRead.cpp) */
#define ADDR_MAP_UNLOAD_ALL 0x006A6710u /* tüm yüklü alanları boşaltır (oyunun ışınlanma yolu) */
#define ADDR_MAP_AREAS      0x00C96088u /* yüklü alan listesi; boşsa dünya yüklü değil */
#define ADDR_WORLD_SNAP     0x00C62954u /* sonraki karede gökyüzü/ışık anında güncellensin */
#define ADDR_MAPOBJ_BUCKETS 0x00CA8054u /* CMapObj hash kovaları (12 bayt: bağlantı ofseti, baş) */
#define ADDR_MAPOBJ_MASK    0x00CA805Cu
/* DBC tabloları (WowClientDB<T>): +0 kayıtlar, +4 kayıt sayısı, +8 kimlik
 * dizini (T**), +0xC en büyük kimlik, +0x10 yüklendi. Her tablonun kendi
 * yükleme fonksiyonu (thiscall, yüklüyse hiçbir şey yapmaz) dosyayı SFile
 * ile okur; kayıtlardaki metinler, kayıtların arkasındaki metin bloğuna
 * işaretçidir. */
#define ADDR_SMEM_FREE  0x00646430u /* stdcall(ptr, ad, satır, bayrak) */
#define ADDR_SMEM_ALLOC 0x006462E0u /* stdcall(boyut, ad, satır, bayrak) */
typedef struct { const char *file; uintptr_t load, table; } DbInfo;
static const DbInfo DBS[] = {
	{"DBFilesClient\\CharSections.dbc",              0x005540A0u, 0x00C0DF64u},
	{"DBFilesClient\\CharHairGeosets.dbc",           0x00541840u, 0x00C0DF78u},
	{"DBFilesClient\\CharacterFacialHairStyles.dbc", 0x00541EF0u, 0x00C0DF28u},
	{"DBFilesClient\\CreatureDisplayInfoExtra.dbc",  0x00542C50u, 0x00C0DE9Cu},
	{"DBFilesClient\\CharVariations.dbc",            0x00541CC0u, 0x00C0DF3Cu},
	{"DBFilesClient\\HelmetGeosetVisData.dbc",       0x00546F00u, 0x00C0DC44u},
	{"DBFilesClient\\CreatureDisplayInfo.dbc",       0x00542E90u, 0x00C0DE88u},
	{"DBFilesClient\\CreatureModelData.dbc",         0x00543300u, 0x00C0DE60u},
	{"DBFilesClient\\CreatureSoundData.dbc",         0x00543540u, 0x00C0DE4Cu},
	{"DBFilesClient\\ItemDisplayInfo.dbc",           0x00547590u, 0x00C0DC08u},
	{"DBFilesClient\\SoundEntries.dbc",              0x0054D9E0u, 0x00C0D8D4u},
	{"DBFilesClient\\NPCSounds.dbc",                 0x0054AFA0u, 0x00C0DA64u},
	{"DBFilesClient\\LightFloatBand.dbc",            0x00549030u, 0x00CE9D88u},
	/* O (Raid Visuals): büyü görselleri; Spell'deki görsel/ikon kimlikleri ve
	 * yalnız O'da olan görsel kayıtları birlikte değişir */
	{"DBFilesClient\\Spell.dbc",                     0x0054FFD0u, 0x00C0D780u},
	{"DBFilesClient\\SpellIcon.dbc",                 0x0054F4B0u, 0x00C0D7E4u},
	{"DBFilesClient\\SpellVisual.dbc",               0x005508E0u, 0x00C0D730u},
	{"DBFilesClient\\SpellVisualEffectName.dbc",     0x00550460u, 0x00C0D758u},
	{"DBFilesClient\\SpellVisualKit.dbc",            0x00550690u, 0x00C0D744u},
};
#define NDBS (sizeof DBS / sizeof DBS[0])
#define DB_RECORDS 0x00
#define DB_COUNT   0x04
#define DB_INDEX   0x08
#define DB_MAXID   0x0C
#define DB_LOADED  0x10

/* VanillaHelpers "ResizeCharacterSkin": T paketindeki işaret dosyasını oyun
 * açılırken görürse karakter ten birleştirmesini 256<<m ölçeğine yamalar
 * (VanillaHelpers.dll 0x1001CE60); ardından birleştirme sistemi (0x476020)
 * global tamponunu bu ölçekle ayırır. 2x'te orijinal tenler yanlış yerleşir,
 * 1x'te HD tenler düşük çözünürlükte kalır; tampon ise çalışırken
 * büyütülemez. Bu yüzden oyun hep en büyük ölçekte açılır (işaret dosyası
 * gizlenmez) ve yalnızca YERLEŞİM sabitleri T açık/kapalıya göre değişir;
 * tamponu ayıran/bırakan sabitler (0x4760A8/AD, 0x476792/97, 0x44AFBA/C6)
 * en büyük ölçekte kalır, böylece hiçbir yazma tamponu aşmaz. Karakterlerin
 * ten dokusu (bileşen+0x184) yeniden kurulurken yeni ölçekle oluşturulur.
 *
 * 4x: işaret dosyası yalnız T'de (2x) olabilir; 4x ten getiren bir paket (Y gibi)
 * işaret taşımaz. Bu yüzden açılışta her paketin ten ölçeği temel ten dokusunun
 * genişliğinden ölçülür (skin_scale_of_pack) ve tampon gerekirse VanillaHelpers'ın
 * seçtiğinden büyük ayrılır. Yerleşim, açık paketlerin en büyük ölçeğidir. */
#define VH_REGION_TABLE  0x00B42450u /* 40 dword, ölçekle kaydırılır */
#define VH_REGION_COUNT  40
#define VH_ALLOC_CONST   0x004760A8u /* birleştirme tamponu boyutu (256<<enBüyük) */
static const uintptr_t VH_LAYOUT_256[] = {0x004778D3u, 0x00477930u, 0x00477935u};
static const uintptr_t VH_LAYOUT_512[] = {0x00477CA1u, 0x00477DD1u, 0x00477F43u};
static const uintptr_t VH_LAYOUT_255[] = {0x00477A10u, 0x00477A15u};
/* tamponu ayıran (0x476020) ve yeniden ayıran (0x476792) sabitler */
static const uintptr_t VH_BUFFER[] = {0x004760A8u, 0x004760ADu, 0x00476792u, 0x00476797u};
/* Doku havuzu (0x44AF90 al, 0x44B110 geri ver) yalnızca 8..256 boyutlar içindir:
 * 0x905844'teki tablo 2 biçim x 6 genişlik x 6 yükseklik (log2(boyut/8) = 0..5)
 * girişlik, hemen ardında 0x905BA0'da başka bir yapı başlar. VanillaHelpers
 * T'nin işaret dosyasını görünce almadaki boyut sınırını (0x44AFBA/C6, cmp edx/edi,
 * 0x100) 256<<m yapar; bu yüzden 512/1024'lük ten tamponu tablonun dışına indeks
 * hesaplar ve oradaki veriyi serbest liste sanıp yazar (ERROR #132, 0x0044B06B,
 * yazılan adres 0x3F3F3F3E). Sınır 256'da kalınca büyük doku havuzu atlar ve
 * doğrudan ayrılır (0x44B0EA -> 0x5A4AF0, havuz boşken izlenen yolun aynısı);
 * geri verme zaten 256 sınırıyla havuza dönmez. Bu iki sabit her açılışta
 * oyunun kendi değerine geri yazılır. */
static const uintptr_t TEXPOOL_LIMIT[] = {0x0044AFBAu, 0x0044AFC6u};
static const uint8_t TEXPOOL_LIMIT_OP[][2] = {{0x81, 0xFA}, {0x81, 0xFF}}; /* cmp edx / cmp edi, imm32 */
#define TEXPOOL_LIMIT_VALUE 0x100u
#define SKIN_SCALE_MAX   2 /* 4x */
#define ADDR_SFILE_READ  0x00656B40u /* stdcall SFileReadFileEx(h, tampon, boyut, &okunan, ov, 0, 0) */
#define ADDR_COMPONENT_INIT_CALL 0x00402B07u /* call CCharacterComponent::Initialize */
#define ADDR_HANDLE_CLOSE  0x0041AED0u /* fastcall(tutamaç) */
#define TYPEMASK_CORPSE    0x80
#define CORPSE_COMPONENT   0x29C
#define COMPONENT_TEXTURE  0x184 /* CharacterBaseSkin dokusu */
#define COMPONENT_DIRTY    0x08
static int g_vhMax = -1;    /* birleştirme tamponunun ölçeği (yerleşim bunu aşamaz) */
static int g_vhMarker = -1; /* VanillaHelpers'ın işaret dosyasından seçtiği ölçek */
static signed char g_packSkin[26] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
                                     -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
static LONG g_skinPacks;    /* ten dokusu getiren paketlerin bitleri */

#define MAPOBJ_HASH  0x04
#define MAPOBJ_NAME  0x1C

typedef int (__fastcall *StormSearch_t)(void *arc, const char *name, uint32_t a1,
                                        uint32_t a2, uint32_t a3, uint32_t a4, uint32_t a5);
typedef void (__fastcall *RegisterFunc_t)(const char *name, void *fn);
typedef void (*LoadScript_t)(void);
typedef int (__stdcall *SFileOpen_t)(void *arc, const char *name, uint32_t scope, void **h);
typedef uint32_t (__stdcall *SFileSize_t)(void *h, uint32_t *high);
typedef int (__stdcall *SFileClose_t)(void *h);
typedef int (__stdcall *SFileRead_t)(void *h, void *buf, uint32_t size, uint32_t *read, void *ov, int a, int b);
typedef int (__thiscall *TexProcess_t)(void *tex);
typedef void (__fastcall *TexFreeImage_t)(void *bits, int format, int w, int h);
typedef uint32_t (__thiscall *GxTexSize_t)(void *dev, void *gx, int w, int h);
typedef void (__thiscall *GxTexDirty_t)(void *dev, void *gx, const int32_t *rect, int immediate);
typedef void (__thiscall *UnitRebuild_t)(void *unit);
/* Takes no stack argument: the game calls it as `mov ecx, obj; call [vt+0x28]`
 * (0x613E3D). Declaring an extra argument shifted the stack by 4 bytes per
 * game object and corrupted locals (crashes at 0x706B8C, HDToggle+0x2998/+0x2A88). */
typedef const char *(__thiscall *ObjModelName_t)(void *obj);
typedef void *(__thiscall *SceneCreate_t)(void *scene, const char *name, int flags);
typedef void (__thiscall *ObjSetModel_t)(void *obj, void *model);
typedef void (__thiscall *ModelRelease_t)(void *model);
typedef void (__thiscall *ModelSetCb_t)(void *model, void *fn, void *arg, int x);
typedef void (__thiscall *ModelSetLight_t)(void *model, void *fn, void *arg);
typedef int (__fastcall *DoodadCreate_t)(const char *name, void *def, int flag);
typedef void (*MapUnloadAll_t)(void);
typedef void (__fastcall *HandleClose_t)(void *h);
typedef void (*ComponentInit_t)(void);
typedef void (__thiscall *DbLoad_t)(void *table);
typedef void (__stdcall *SMemFree_t)(void *ptr, const char *name, int line, int flags);
typedef void *(__stdcall *SMemAlloc_t)(uint32_t size, const char *name, int line, int flags);
typedef void (__fastcall *GxTexRelease_t)(void *gx);
typedef int (__stdcall *SFile2Open_t)(int zero, const char *name, int flag, void **h);
typedef uint32_t (__stdcall *SFile2Size_t)(void *h, uint32_t *high);
typedef int (__stdcall *SFile2Read_t)(void *h, void *buf, uint32_t size, uint32_t *read, void *ov, int unused);
typedef int (__stdcall *SFile2Close_t)(void *h);

typedef int (__fastcall *LuaGettop_t)(void *L);
typedef double (__fastcall *LuaTonumber_t)(void *L, int idx);
typedef const char *(__fastcall *LuaTostring_t)(void *L, int idx);
typedef void (__fastcall *LuaPushnil_t)(void *L);
typedef void (__fastcall *LuaPushnumber_t)(void *L, double n);
typedef void (__fastcall *LuaPushstring_t)(void *L, const char *s);
typedef void (__fastcall *LuaPushboolean_t)(void *L, int b);

#define lua_gettop(L)        ((LuaGettop_t)ADDR_LUA_GETTOP)(L)
#define lua_tonumber(L, i)   ((LuaTonumber_t)ADDR_LUA_TONUMBER)(L, i)
#define lua_tostring(L, i)   ((LuaTostring_t)ADDR_LUA_TOSTRING)(L, i)
#define lua_pushnil(L)       ((LuaPushnil_t)ADDR_LUA_PUSHNIL)(L)
#define lua_pushnumber(L, n) ((LuaPushnumber_t)ADDR_LUA_PUSHNUMBER)(L, n)
#define lua_pushstring(L, s) ((LuaPushstring_t)ADDR_LUA_PUSHSTRING)(L, s)
#define lua_pushboolean(L, b) ((LuaPushboolean_t)ADDR_LUA_PUSHBOOLEAN)(L, b)

/* Bu paketler hiç kapatılamaz: Z, OctoWoW'un DBC birleştirme yaması. Z'nin
 * dosyaları, aynı dosyayı içeren paketlerden biri kapalıyken yok sayılır. */
#define MERGE_PACK_BIT ('Z' - 'A')
/* H: OctoWoW HD fixes. Its files are corrected copies of files from the other packs (or
 * textures those files need); when a pack that has the same file is off, the corrected copy
 * is skipped too, so the file falls back exactly as the rest of that pack does.
 * V: the same for Project Reforged's Ultra HD pack U (it loads above U, H loads below it). */
#define FIX_PACKS ((1L << ('H' - 'A')) | (1L << ('V' - 'A')))
static const char PROTECTED[] = "Z";

static HMODULE g_self;
static char g_dir[MAX_PATH];        /* DLL klasörü, sonda '\' ile */
static volatile LONG g_offMask;     /* bit i: 'A'+i kapalı (dosya çözmede geçerli olan) */
/* İstenen seçim. HDPack_Set yalnızca bunu değiştirir; HDPack_Refresh onu
 * g_offMask'e uygular ve aynı çağrıda her şeyi yeniler. Böylece dosyaların yeni
 * seçimle okunduğu ama tabloların (DBC) henüz eski kaldığı bir ara an olmaz. */
static volatile LONG g_wantMask;
static LONG g_appliedMask;          /* ekrandaki dokuların yüklendiği seçim */
static StormSearch_t g_stormSearch; /* orijinal arama (trampolin) */
static LoadScript_t g_loadScript;   /* çağrı noktasındaki asıl hedef */
static int g_hooksOk;
static char g_status[160] = "not loaded";
static CRITICAL_SECTION g_logLock;

/* --- günlük -------------------------------------------------------------- */
static void hdlog(const char *fmt, ...)
{
	char path[MAX_PATH + 32], line[512];
	SYSTEMTIME t;
	va_list ap;
	FILE *f;
	GetLocalTime(&t);
	int n = snprintf(line, sizeof line, "%02d:%02d:%02d ", t.wHour, t.wMinute, t.wSecond);
	va_start(ap, fmt);
	vsnprintf(line + n, sizeof line - n, fmt, ap);
	va_end(ap);
	snprintf(path, sizeof path, "%sHDToggle.log", g_dir);
	EnterCriticalSection(&g_logLock);
	if ((f = fopen(path, "a")) != NULL) {
		fprintf(f, "%s\n", line);
		fclose(f);
	}
	LeaveCriticalSection(&g_logLock);
}

/* --- teşhis günlüğü ------------------------------------------------------------
 * HDToggle.ini [HDToggle] debug=0|1|2 (varsayılan 0: yayın davranışı).
 * 1: her yenilemenin ayrıntısı (her doku, ten parçası, model ve karakter; aşama süreleri),
 *    açılışta yüklü modüller ve dlls.txt.
 * 2: ayrıca her ten parçası birleştirmesi ve her dosya yönlendirmesi.
 * Ayrıntı satırları yenileme başına bir bütçeyle sınırlıdır (günlük şişmesin). */
static int g_debug;
static volatile LONG g_dbgBudget1, g_dbgBudget2;
#define DBG_BUDGET1 6000
#define DBG_BUDGET2 4000
#define dbg1(...) do { if (g_debug >= 1 && InterlockedDecrement(&g_dbgBudget1) >= 0) hdlog("D1 " __VA_ARGS__); } while (0)
#define dbg2(...) do { if (g_debug >= 2 && InterlockedDecrement(&g_dbgBudget2) >= 0) hdlog("D2 " __VA_ARGS__); } while (0)

static void dbg_reset_budget(void)
{
	InterlockedExchange(&g_dbgBudget1, DBG_BUDGET1);
	InterlockedExchange(&g_dbgBudget2, DBG_BUDGET2);
}

/* --- bellek ölçümü --------------------------------------------------------------
 * 32 bit oyunun adres alanı (LAA ile 4 GB, yoksa 2 GB) dolunca oyun "Not enough memory
 * resources" ile kapanır (Storm SFile.cpp 1395, M2.cpp 111 gibi rastgele bir ayırmada).
 * Kullanılan alan, özel ayrılmış (commit) bellek ve en büyük boş blok günlüğe yazılır:
 * açılışta, her yenilemenin öncesinde/sonrasında ve belirli aralıklarla. Yenileme
 * öncesi/sonrası farkı, aç/kapat'ın bellek bırakıp bırakmadığını gösterir. */
typedef struct { uint32_t used, commit, largest, total; } MemUsage; /* MB */

static MemUsage mem_usage(void)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uintptr_t lo = (uintptr_t)si.lpMinimumApplicationAddress, hi = (uintptr_t)si.lpMaximumApplicationAddress;
	uint64_t freeB = 0, commit = 0, largest = 0;
	MEMORY_BASIC_INFORMATION m;
	for (uintptr_t a = lo; a < hi && VirtualQuery((void *)a, &m, sizeof m) == sizeof m;) {
		if (m.State == MEM_FREE) {
			freeB += m.RegionSize;
			if (m.RegionSize > largest)
				largest = m.RegionSize;
		} else if (m.State == MEM_COMMIT && m.Type == MEM_PRIVATE) {
			commit += m.RegionSize;
		}
		uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
		if (next <= a)
			break;
		a = next;
	}
	uint64_t total = (uint64_t)(hi - lo) + 1;
	MemUsage u = {(uint32_t)((total - freeB) >> 20), (uint32_t)(commit >> 20), (uint32_t)(largest >> 20),
	              (uint32_t)(total >> 20)};
	return u;
}

/* Büyük ayırmaların (aynı AllocationBase'e ait bölgelerin toplamı) önceki ölçümle farkı:
 * yeni, kalkan ya da 4 MB'tan fazla değişen ayırmalar türü, sahibi ve ilk baytlarıyla
 * yazılır. Aç/kapat sırasında kimin büyük blok ayırıp bıraktığını gösterir. */
#define BIGMAP_MIN (4u << 20)
#define BIGMAP_MAX 512
typedef struct { uintptr_t base; uint32_t size, commit, type; } BigAlloc;
static BigAlloc g_big[BIGMAP_MAX];
static int g_nbig = -1;
static SRWLOCK g_bigLock = SRWLOCK_INIT;

static int big_snapshot(BigAlloc *out)
{
	SYSTEM_INFO si;
	GetSystemInfo(&si);
	uintptr_t hi = (uintptr_t)si.lpMaximumApplicationAddress;
	int n = 0;
	BigAlloc cur = {0, 0, 0, 0};
	MEMORY_BASIC_INFORMATION m;
	for (uintptr_t a = (uintptr_t)si.lpMinimumApplicationAddress; a < hi && VirtualQuery((void *)a, &m, sizeof m) == sizeof m;) {
		uintptr_t base = m.State == MEM_FREE ? 0 : (uintptr_t)m.AllocationBase;
		if (base != cur.base) {
			if (cur.base && cur.size >= BIGMAP_MIN && n < BIGMAP_MAX)
				out[n++] = cur;
			cur.base = base, cur.size = 0, cur.commit = 0, cur.type = m.Type;
		}
		if (base) {
			cur.size += (uint32_t)m.RegionSize;
			if (m.State == MEM_COMMIT)
				cur.commit += (uint32_t)m.RegionSize;
		}
		uintptr_t next = (uintptr_t)m.BaseAddress + m.RegionSize;
		if (next <= a)
			break;
		a = next;
	}
	if (cur.base && cur.size >= BIGMAP_MIN && n < BIGMAP_MAX)
		out[n++] = cur;
	return n;
}

static void big_describe(const BigAlloc *b, char *out, size_t cap)
{
	const char *type = b->type == MEM_IMAGE ? "image" : b->type == MEM_MAPPED ? "mapped" : "private";
	char owner[MAX_PATH] = "";
	if (b->type == MEM_IMAGE) {
		GetModuleFileNameA((HMODULE)b->base, owner, sizeof owner);
	} else if (b->type == MEM_MAPPED) {
		typedef DWORD (WINAPI *MappedName_t)(HANDLE, LPVOID, LPSTR, DWORD);
		MappedName_t f = (MappedName_t)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetMappedFileNameA");
		if (!f || !f(GetCurrentProcess(), (void *)b->base, owner, sizeof owner))
			strcpy(owner, "no file");
	}
	char head[40] = "";
	uint32_t w[4];
	SIZE_T got = 0; /* ReadProcessMemory: bölge bu arada bırakılmış olsa da güvenli */
	if (ReadProcessMemory(GetCurrentProcess(), (void *)b->base, w, sizeof w, &got) && got == sizeof w) {
		snprintf(head, sizeof head, " [%08X %08X %08X %08X]", (unsigned)w[0], (unsigned)w[1], (unsigned)w[2], (unsigned)w[3]);
	}
	const char *slash = strrchr(owner, '\\');
	snprintf(out, cap, "0x%08X %u MB (%u MB committed) %s%s%s%s", (unsigned)b->base, b->size >> 20, b->commit >> 20,
	         type, *owner ? " " : "", slash ? slash + 1 : owner, head);
}

static void big_diff(void)
{
	static BigAlloc now[BIGMAP_MAX];
	char d[MAX_PATH + 128];
	AcquireSRWLockExclusive(&g_bigLock);
	int n = big_snapshot(now);
	if (g_nbig < 0) {
		for (int i = 0; i < n; i++) {
			big_describe(&now[i], d, sizeof d);
			hdlog("  allocation %s", d);
		}
	} else {
		for (int i = 0; i < n; i++) {
			int j = 0;
			while (j < g_nbig && g_big[j].base != now[i].base)
				j++;
			big_describe(&now[i], d, sizeof d);
			if (j == g_nbig)
				hdlog("  + new %s", d);
			else if (now[i].size != g_big[j].size || now[i].commit + BIGMAP_MIN <= g_big[j].commit ||
			         g_big[j].commit + BIGMAP_MIN <= now[i].commit)
				hdlog("  ~ was %u MB (%u MB committed), now %s", g_big[j].size >> 20, g_big[j].commit >> 20, d);
		}
		for (int j = 0; j < g_nbig; j++) {
			int i = 0;
			while (i < n && now[i].base != g_big[j].base)
				i++;
			if (i == n) {
				big_describe(&g_big[j], d, sizeof d);
				hdlog("  - gone %s", d);
			}
		}
	}
	memcpy(g_big, now, (size_t)n * sizeof now[0]);
	g_nbig = n;
	ReleaseSRWLockExclusive(&g_bigLock);
}

/* Model önbelleğindeki modeller ve bunların eskimiş (silinmeyi bekleyen) olanları.
 * Önbellek ana iş parçacığında değişir: yalnızca oradan (yenileme öncesi/sonrası) çağrılır. */
static void model_counts(int *all, int *stale)
{
	*all = *stale = 0;
	for (int b = 0; b < M2CACHE_NBUCKETS; b++)
		for (uint8_t *sh = *(uint8_t **)(ADDR_M2CACHE_BUCKETS + b * 4); sh && *all < 1000000; sh = *(uint8_t **)(sh + M2S_NEXT)) {
			++*all;
			if (sh[M2S_NAME] == M2_STALE_MARK)
				++*stale;
		}
}

static void memlog(const char *when)
{
	MemUsage u = mem_usage();
	uint8_t *dev = *(uint8_t **)ADDR_GX_DEVICE;
	hdlog("memory %s: %u/%u MB address space used, %u MB private, largest free block %u MB; game texture memory %u MB",
	      when, u.used, u.total, u.commit, u.largest, dev ? *(uint32_t *)(dev + GXDEV_TEXMEM) >> 20 : 0);
	if (strstr(when, "refresh")) {
		int all, stale;
		model_counts(&all, &stale);
		hdlog("  models cached %d, of them stale (waiting to be freed) %d; stale models freed right away so far %d", all, stale,
		      g_staleFreed);
	}
	if (g_debug && strcmp(when, "at start"))
		big_diff();
	if (u.largest < 64 || u.used > u.total / 100 * 92)
		hdlog("WARNING: the game's address space is nearly full; it may close with \"Not enough memory "
		      "resources\" (fewer HD packs, especially Ultra T/U, use less)");
}

/* Belirli aralıklarla ölçüm: ayrı bir iş parçacığı DLL'i bellekte tutardı; bunun yerine
 * her dosya aramasında süre dolmuş mu bakılır, ölçümü yalnızca süreyi ilk alan çağrı yapar. */
static volatile LONG g_memNext;
static void mem_tick(void)
{
	LONG next = g_memNext;
	DWORD now = GetTickCount();
	if ((LONG)(now - (DWORD)next) >= 0 &&
	    InterlockedCompareExchange(&g_memNext, (LONG)(now + (g_debug ? 60000u : 300000u)), next) == next)
		memlog("now");
}

/* arşivin dosya adı (patch-A.mpq) ya da "-" (yok/silinmiş) */
static const char *arc_label(void *arc)
{
	if (!arc)
		return "-";
	const char *p = (const char *)arc + ARC_NAME, *b = p;
	for (; *p; p++)
		if (*p == '\\' || *p == '/')
			b = p + 1;
	return b;
}

/* --- server check ------------------------------------------------------------
 * HD Switch works on every server except the ones listed here (Capycraft, Ravencraft).
 * The realm list address (realmList CVar, which the game loads from realmlist.wtf)
 * must not be in a listed domain, and the selected realm (realmName CVar) must not
 * be one of their realms: a login proxy (local address) or a bare IPv4 address
 * hides the domain, the realm name still tells. OctoWoW's own domain and realms
 * are always allowed. Names are compared as salted hashes, so the DLL holds no
 * plain domain or realm name to edit. On a listed server the DLL locks itself:
 * every pack stays as installed (on), the saved selection is not applied and the
 * Lua API refuses changes. */
#define ADDR_CVAR_REALMLIST 0x00C2812Cu /* CVar* realmList (0x5AB680) */
#define ADDR_CVAR_REALMNAME 0x00C28130u /* CVar* realmName (0x5AB7D0) */
#define CVAR_VALUE          0x20        /* const char* current value */
#define NAME_HASH_SEED      0x6B1D93E5u
static const uint32_t OK_DOMAIN = 0x8ECA3A76u;
static const uint32_t OK_REALMS[] = {0x6C77EC91u, 0xE560D97Cu, 0xD5B3393Cu};
static const uint32_t BLOCKED_DOMAINS[] = {0x7B636D6Au, 0x93752FF1u};
static const uint32_t BLOCKED_REALMS[] = {0xF9D6C9E7u, 0x27ADA730u, 0x87B95FE4u, 0x3B09DC4Eu};
static volatile LONG g_locked;      /* 1: a listed server */
static char g_fileHost[128], g_fileRealm[64]; /* from realmlist.wtf / Config.wtf at startup */

static uint32_t name_hash(const char *s, size_t n)
{
	uint32_t h = NAME_HASH_SEED;
	for (size_t i = 0; i < n && s[i]; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c >= 'A' && c <= 'Z')
			c += 32;
		h = (h ^ c) * 0x01000193u;
	}
	return h;
}

/* "host[:port]" -> host, lower case, without quotes/spaces */
static void host_of(const char *in, char *out, size_t size)
{
	size_t n = 0;
	while (*in == ' ' || *in == '"')
		in++;
	for (; *in && *in != ':' && *in != '"' && *in != ' ' && *in != '\r' && *in != '\n' && n + 1 < size; in++)
		out[n++] = (char)((*in >= 'A' && *in <= 'Z') ? *in + 32 : *in);
	out[n] = 0;
}

static int hash_listed(uint32_t h, const uint32_t *list, size_t n)
{
	for (size_t i = 0; i < n; i++)
		if (h == list[i])
			return 1;
	return 0;
}

/* hash of the last two labels ("name.tld"), 0 if there is no dot */
static uint32_t domain_hash(const char *h)
{
	const char *last = strrchr(h, '.'), *dom = h;
	if (!last)
		return 0;
	for (const char *p = h; p < last; p++)
		if (*p == '.')
			dom = p + 1;
	return name_hash(dom, strlen(dom));
}

/* 1: allowed. An unknown address or a realm not chosen yet is allowed. */
static int server_verdict(const char *address, const char *realm)
{
	char host[128] = "";
	if (address)
		host_of(address, host, sizeof host);
	uint32_t dom = domain_hash(host);
	if (dom == OK_DOMAIN)
		return 1;
	if (hash_listed(dom, BLOCKED_DOMAINS, sizeof BLOCKED_DOMAINS / sizeof BLOCKED_DOMAINS[0]))
		return 0;
	if (realm && realm[0]) {
		uint32_t r = name_hash(realm, strlen(realm));
		if (!hash_listed(r, OK_REALMS, sizeof OK_REALMS / sizeof OK_REALMS[0]) &&
		    hash_listed(r, BLOCKED_REALMS, sizeof BLOCKED_REALMS / sizeof BLOCKED_REALMS[0]))
			return 0;
	}
	return 1;
}

static const char *cvar_value(uintptr_t global)
{
	uint8_t *cv = *(uint8_t **)global;
	return cv ? *(const char **)(cv + CVAR_VALUE) : NULL;
}

static int server_ok_now(void)
{
	const char *host = cvar_value(ADDR_CVAR_REALMLIST), *realm = cvar_value(ADDR_CVAR_REALMNAME);
	return server_verdict(host ? host : g_fileHost, realm ? realm : g_fileRealm);
}

/* value of `set <key> "<value>"` in a WTF file (case-insensitive key) */
static void wtf_value(const char *path, const char *key, char *out, size_t size)
{
	char line[512];
	size_t klen = strlen(key);
	FILE *f = fopen(path, "r");
	out[0] = 0;
	if (!f)
		return;
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (*p == ' ' || *p == '\t')
			p++;
		if (_strnicmp(p, "set ", 4))
			continue;
		p += 4;
		while (*p == ' ')
			p++;
		if (_strnicmp(p, key, klen) || (p[klen] != ' ' && p[klen] != '\t'))
			continue;
		p += klen;
		while (*p == ' ' || *p == '\t')
			p++;
		if (*p == '"')
			p++;
		size_t n = 0;
		while (*p && *p != '"' && *p != '\r' && *p != '\n' && n + 1 < size)
			out[n++] = *p++;
		out[n] = 0;
	}
	fclose(f);
}

/* game folder: the DLL lives in <game>\mods\ */
static void game_path(char *out, size_t size, const char *file)
{
	char dir[MAX_PATH];
	snprintf(dir, sizeof dir, "%s", g_dir);
	size_t n = strlen(dir);
	if (n >= 5 && !_stricmp(dir + n - 5, "mods\\"))
		dir[n - 5] = 0;
	snprintf(out, size, "%s%s", dir, file);
}

static void read_server_files(void)
{
	char path[MAX_PATH + 64];
	game_path(path, sizeof path, "realmlist.wtf");
	wtf_value(path, "realmlist", g_fileHost, sizeof g_fileHost);
	game_path(path, sizeof path, "WTF\\Config.wtf");
	if (!g_fileHost[0])
		wtf_value(path, "realmList", g_fileHost, sizeof g_fileHost);
	wtf_value(path, "realmName", g_fileRealm, sizeof g_fileRealm);
}

/* --- update check -------------------------------------------------------------------
 * Once per game start, a few seconds after loading, a background thread asks
 * GitHub for the latest release tag. Nothing about the player is sent; only a
 * strict "digits and dots" tag is accepted, and the download link shown to the
 * player is fixed in the addon, never taken from the answer. [HDToggle]
 * updatecheck=0 in HDToggle.ini turns it off; it never runs when locked. */
#define UPDATE_URL "https://api.github.com/repos/fmustafayaman/OctoWoW-HD-Switch/releases/latest"
static void ini_path_early(char *out, size_t size);
static volatile LONG g_updateDone;  /* 1: the check finished (successfully or not) */
static char g_latest[16];           /* newer version than this one, "" if none */

/* "1", "1.2" or "1.2.3" (up to 3 digits each) -> comparable number; -1 if malformed */
static long version_number(const char *v)
{
	long parts[3] = {0, 0, 0};
	int n = 0, digits = 0;
	for (const char *p = v;; p++) {
		if (*p >= '0' && *p <= '9') {
			if (++digits > 3)
				return -1;
			parts[n] = parts[n] * 10 + (*p - '0');
		} else if (*p == '.' || !*p) {
			if (!digits)
				return -1;
			n++, digits = 0;
			if (!*p)
				break;
			if (n == 3)
				return -1;
		} else {
			return -1;
		}
	}
	return parts[0] * 1000000 + parts[1] * 1000 + parts[2];
}

/* tag value from a GitHub release JSON: "tag_name":"v1.2.3" -> "1.2.3" */
static int parse_tag(const char *json, char *out, size_t size)
{
	const char *p = strstr(json, "\"tag_name\"");
	if (!p || !(p = strchr(p + 10, ':')))
		return 0;
	while (*++p == ' ')
		;
	if (*p++ != '"')
		return 0;
	if (*p == 'v' || *p == 'V')
		p++;
	size_t n = 0;
	while (p[n] && p[n] != '"' && n + 1 < size)
		n++;
	if (p[n] != '"' || !n)
		return 0;
	memcpy(out, p, n);
	out[n] = 0;
	return version_number(out) >= 0;
}

static DWORD WINAPI update_thread(void *arg) /* arg: this DLL, referenced for the thread's lifetime */
{
	char path[MAX_PATH + 32], url[300], buf[16384], tag[16];
	ini_path_early(path, sizeof path);
	GetPrivateProfileStringA("HDToggle", "updateurl", UPDATE_URL, url, sizeof url, path);
	Sleep(GetPrivateProfileIntA("HDToggle", "updatedelay", 5000, path));
	HINTERNET net = InternetOpenA("HDToggle/" HDT_VERSION, INTERNET_OPEN_TYPE_PRECONFIG, NULL, NULL, 0);
	HINTERNET req = NULL;
	DWORD got = 0, n = 0;
	if (net) {
		DWORD ms = 8000;
		InternetSetOptionA(net, INTERNET_OPTION_CONNECT_TIMEOUT, &ms, sizeof ms);
		InternetSetOptionA(net, INTERNET_OPTION_RECEIVE_TIMEOUT, &ms, sizeof ms);
		req = InternetOpenUrlA(net, url, "Accept: application/vnd.github+json\r\n", (DWORD)-1,
		                       INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_COOKIES |
		                       INTERNET_FLAG_NO_UI | (strncmp(url, "https:", 6) ? 0 : INTERNET_FLAG_SECURE), 0);
	}
	if (req)
		while (got + 1 < sizeof buf && InternetReadFile(req, buf + got, sizeof buf - 1 - got, &n) && n)
			got += n;
	buf[got] = 0;
	if (req)
		InternetCloseHandle(req);
	if (net)
		InternetCloseHandle(net);
	if (!got || !parse_tag(buf, tag, sizeof tag)) {
		hdlog("update check: no answer (%lu)", req ? 0ul : GetLastError());
	} else if (version_number(tag) > version_number(HDT_VERSION)) {
		snprintf(g_latest, sizeof g_latest, "%s", tag);
		hdlog("update check: version %s is available (this is %s)", tag, HDT_VERSION);
	} else {
		hdlog("update check: up to date (%s)", HDT_VERSION);
	}
	InterlockedExchange(&g_updateDone, 1);
	FreeLibraryAndExitThread((HMODULE)arg, 0);
	return 0;
}

/* --- ayar dosyası ---------------------------------------------------------- */
static int letter_bit(char c)
{
	if (c >= 'a' && c <= 'z')
		c -= 32;
	if (c < 'A' || c > 'Z' || strchr(PROTECTED, c))
		return -1;
	return c - 'A';
}

static void mask_to_letters(LONG mask, char *out)
{
	int n = 0;
	for (int i = 0; i < 26; i++)
		if (mask & (1L << i))
			out[n++] = (char)('A' + i);
	out[n] = 0;
}

static void ini_path(char *out, size_t size, const char *file)
{
	snprintf(out, size, "%s%s", g_dir, file);
}

static void ini_path_early(char *out, size_t size) { ini_path(out, size, "HDToggle.ini"); }

static void load_settings(void)
{
	char path[MAX_PATH + 32], buf[64];
	LONG mask = 0;
	ini_path(path, sizeof path, "HDToggle.ini");
	GetPrivateProfileStringA("HDToggle", "off", "", buf, sizeof buf, path);
	if (g_locked)
		buf[0] = 0; /* listed server: everything stays as installed */
	for (char *p = buf; *p; p++) {
		int b = letter_bit(*p);
		if (b >= 0)
			mask |= 1L << b;
	}
	InterlockedExchange(&g_offMask, mask);
	InterlockedExchange(&g_wantMask, mask);
	g_appliedMask = mask;
	g_debug = (int)GetPrivateProfileIntA("HDToggle", "debug", 0, path);
	dbg_reset_budget();
}

static void save_settings(void)
{
	char path[MAX_PATH + 32], letters[27];
	ini_path(path, sizeof path, "HDToggle.ini");
	mask_to_letters(g_wantMask, letters);
	WritePrivateProfileStringA("HDToggle", "off", letters, path);
}

/* --- Storm arşiv listesi ----------------------------------------------- */
typedef struct {
	void *arc;
	int bit; /* paket harfi biti, paket değilse -1 */
	int prio;
} ArcInfo;

/* Data\patch-X.mpq -> X'in biti; diğerleri -1 */
static int archive_bit(const char *path)
{
	const char *base = path, *p;
	for (p = path; *p; p++)
		if (*p == '\\' || *p == '/')
			base = p + 1;
	if (strlen(base) != 11 || _strnicmp(base, "patch-", 6) != 0 || _stricmp(base + 7, ".mpq") != 0)
		return -1;
	char c = base[6];
	if (c >= 'a' && c <= 'z')
		c -= 32;
	if (c < 'A' || c > 'Z')
		return -1;
	return c - 'A';
}

/* Listenin anlık kopyası (öncelik sırasıyla). Kilit yalnızca okuma boyunca
 * tutulur; arama çağrıları kilitsiz yapılır (Storm her tutamacı zaten
 * listeye karşı doğruluyor). */
static int snapshot(ArcInfo *out, int max)
{
	int n = 0;
	EnterCriticalSection((LPCRITICAL_SECTION)ADDR_ARCHIVE_LOCK);
	uintptr_t a = *(uintptr_t *)ADDR_ARCHIVE_HEAD;
	while (a && !(a & 1) && n < max) {
		out[n].arc = (void *)a;
		out[n].bit = archive_bit((const char *)(a + ARC_NAME));
		out[n].prio = *(int *)(a + ARC_PRIO);
		n++;
		a = *(uintptr_t *)(a + ARC_NEXT);
	}
	LeaveCriticalSection((LPCRITICAL_SECTION)ADDR_ARCHIVE_LOCK);
	return n;
}

/* Tek arşivde arama: 0 yok, 1 var, 3 bu arşivde "silindi" işareti var
 * (yamalar dosya silebilir; Storm genel aramada bunu "yok" diye bitirir). */
static int archive_lookup(void *arc, const char *name)
{
	int r = g_stormSearch(arc, name, 0, 0, 0, 0, 0);
	return r == 1 || r == 3 ? r : 0;
}

/* Genel aramada dosyanın okunacağı arşivi seçer. NULL: doğal genel arama
 * aynen kalsın; aksi halde istek o arşive yönlendirilir. */
static void *resolve(const char *name, LONG mask)
{
	ArcInfo list[64];
	int n = snapshot(list, 64), i, inOff = 0;

	/* 1) Kapalı paketlerden birinde (dosya ya da silme işareti olarak) var mı?
	 *    Yoksa hiçbir şey değişmez. */
	for (i = 0; i < n && !inOff; i++)
		if (list[i].bit >= 0 && (mask & (1L << list[i].bit)) && archive_lookup(list[i].arc, name))
			inOff = 1;
	if (!inOff)
		return NULL;
	/* Z, diğer paketlerin DBC'lerini birleştiren yamadır (ör. C+O'nun
	 * CreatureModelData'sı, S'nin SoundEntries'i): aynı dosyayı içeren bir paket
	 * kapalıysa Z'nin birleşik sürümü de geçersizdir. H ve V (düzeltmeler) de aynı:
	 * düzelttiği dosyanın paketi kapalıysa düzeltilmiş kopya da atlanır. */
	mask |= (1L << MERGE_PACK_BIT) | FIX_PACKS;

	/* 2) Öncelik sırasıyla ilk sahibi bul. Açık bir arşivse doğal sonuç
	 *    zaten doğru; kapalıysa bir alttaki açık sahibe yönlendir (o arşivde
	 *    silme işareti varsa sonuç da "yok" olur, tıpkı paket hiç yokmuş gibi).
	 *    Açık sahip yoksa dosya yalnızca pakette var: yine paketten okunur. */
	int firstOff = 0;
	for (i = 0; i < n; i++) {
		if (!archive_lookup(list[i].arc, name))
			continue;
		if (list[i].bit < 0 || !(mask & (1L << list[i].bit)))
			return firstOff ? list[i].arc : NULL;
		firstOff = 1;
	}
	return NULL;
}

/* --- arama kancası ------------------------------------------------------- */
static int __fastcall hook_search(void *arc, const char *name, uint32_t a1, uint32_t a2,
                                  uint32_t a3, uint32_t a4, uint32_t a5)
{
	LONG mask = g_offMask;
	mem_tick();
	if (!arc && mask && name && *name) {
		arc = resolve(name, mask);
		if (arc)
			dbg2("file %s -> %s (a pack that has it is off)", name, arc_label(arc));
	}
	return g_stormSearch(arc, name, a1, a2, a3, a4, a5);
}

/* --- Lua API ----------------------------------------------------------------- */

/* HDPack_List() -> "A", true, "B", false, ...  (yüklü harfli paketler, harf sırasıyla) */
static int enforce_server(void);

static int __fastcall lua_HDPack_List(void *L)
{
	if (!enforce_server())
		return 0;
	ArcInfo list[64];
	int n = snapshot(list, 64), count = 0;
	LONG mask = g_wantMask;
	for (int b = 0; b < 26; b++) {
		if (strchr(PROTECTED, 'A' + b))
			continue;
		for (int i = 0; i < n; i++) {
			if (list[i].bit != b)
				continue;
			char s[2] = {(char)('A' + b), 0};
			lua_pushstring(L, s);
			lua_pushboolean(L, !(mask & (1L << b)));
			count += 2;
			break;
		}
	}
	return count;
}

/* HDPack_Set("B", 0|1) -> true | nil */
static int __fastcall lua_HDPack_Set(void *L)
{
	const char *s = lua_gettop(L) >= 2 ? lua_tostring(L, 1) : NULL;
	int b = s && s[0] && !s[1] ? letter_bit(s[0]) : -1;
	if (b < 0 || !g_hooksOk || !enforce_server()) {
		lua_pushnil(L);
		return 1;
	}
	int on = lua_tonumber(L, 2) != 0;
	LONG old, val;
	do {
		old = g_wantMask;
		val = on ? old & ~(1L << b) : old | (1L << b);
	} while (InterlockedCompareExchange(&g_wantMask, val, old) != old);
	save_settings();
	hdlog("pack %c %s", 'A' + b, on ? "on" : "off");
	lua_pushboolean(L, 1);
	return 1;
}

/* Dosyanın verilen seçimde okunacağı arşiv (NULL: bulunamadı / silinmiş). */
static void *effective_owner(const char *name, LONG mask)
{
	void *a = resolve(name, mask);
	if (a)
		return a;
	ArcInfo list[64];
	int n = snapshot(list, 64);
	for (int i = 0; i < n; i++) {
		int r = archive_lookup(list[i].arc, name);
		if (r)
			return r == 1 ? list[i].arc : NULL;
	}
	return NULL;
}

/* Dosya, iki seçim arasında değişen paketlerin birinde var mı? */
static int touched_by(const char *name, LONG changed)
{
	ArcInfo list[64];
	int n = snapshot(list, 64);
	for (int i = 0; i < n; i++)
		if (list[i].bit >= 0 && (changed & (1L << list[i].bit)) && archive_lookup(list[i].arc, name))
			return 1;
	return 0;
}

/* Eski GxTex nesnesini (old) yeni dokunun (fresh) tanımıyla yerinde yeniden
 * kurar. Nesnenin adresi değişmez: arayüz çizim grupları gibi GxTex
 * işaretçisini saklayan yerler aynı nesneyi kullanmaya devam eder. D3D
 * dokusu, oyunun doku kalitesi değişince yaptığı gibi bırakılır, yer tutucu
 * konur ve doku kirli işaretlenir; ilk kullanımda yeni boyutla yeniden
 * oluşturulup geri çağırmayla (dosyadan) doldurulur. */
static void rebuild_gxtex_in_place(uint8_t *old, const uint8_t *fresh)
{
	uint8_t *dev = *(uint8_t **)ADDR_GX_DEVICE;
	uint32_t before = ((GxTexSize_t)ADDR_GXDEV_TEXSIZE)(dev, old, -1, -1);
	void *d3d = *(void **)(old + GXTEX_D3D);
	if (d3d && !old[GXTEX_RECREATE])
		(*(void (__stdcall ***)(void *))d3d)[2](d3d); /* IUnknown::Release */
	memcpy(old + GXTEX_DESC, fresh + GXTEX_DESC, 7 * 4);
	int32_t *rect = (int32_t *)(old + GXTEX_RECT);
	rect[0] = 0, rect[1] = 0, rect[2] = *(int32_t *)(fresh + GXTEX_DESC + 4), rect[3] = *(int32_t *)(fresh + GXTEX_DESC);
	*(int16_t *)(old + GXTEX_W1A) = -1, *(int16_t *)(old + GXTEX_W1A + 2) = -1;
	*(void **)(old + GXTEX_D3D) = *(void **)(dev + GXDEV_TEXSTUB);
	old[GXTEX_RECREATE] = 1;
	old[GXTEX_FLAGSDIRTY] = 1;
	uint32_t after = ((GxTexSize_t)ADDR_GXDEV_TEXSIZE)(dev, old, -1, -1);
	*(uint32_t *)(dev + GXDEV_TEXMEM) += after - before;
	static const int32_t whole[4] = {0, 0, 0, 0};
	((GxTexDirty_t)ADDR_GXDEV_TEXDIRTY)(dev, old, whole, 0);
}

/* Bir dokuyu yeni dosyasına göre yeniden kurar: dosya güncel seçimle okunur,
 * oyunun BLP işleme fonksiyonu geçici bir GxTex oluşturur (yeni boyut,
 * biçim, bayrak); tanımı eski GxTex'e aktarılır ve geçici olan bırakılır. */
static int reload_texture(uint8_t *tex)
{
	const char *name = (const char *)(tex + TEX_NAME);
	uint8_t *oldGx = *(uint8_t **)(tex + TEX_GX);
	void *h = NULL;
	if (!oldGx || !*(void **)ADDR_GX_DEVICE)
		return 0;
	if (!((SFile2Open_t)ADDR_SFILE2_OPEN)(0, name, (*(uint32_t *)(tex + TEX_FLAGS) & 2) != 0, &h) || !h)
		return 0;
	uint32_t hi = 0, size = ((SFile2Size_t)ADDR_SFILE2_SIZE)(h, &hi), got = 0;
	uint8_t *buf = size && !hi ? HeapAlloc(GetProcessHeap(), 0, size) : NULL;
	int ok = buf && ((SFile2Read_t)ADDR_SFILE2_READ)(h, buf, size, &got, 0, 0) && got == size;
	((SFile2Close_t)ADDR_SFILE2_CLOSE)(h);
	if (!ok) {
		if (buf)
			HeapFree(GetProcessHeap(), 0, buf);
		return 0;
	}
	uint32_t savedFlags = *(uint32_t *)(tex + TEX_FLAGS), savedGxFlags = *(uint32_t *)(tex + TEX_GXFLAG);
	*(uint32_t *)(tex + TEX_FLAGS) &= ~1u;
	*(uint32_t *)(tex + TEX_GXFLAG) &= ~0x40u;
	*(void **)(tex + TEX_GX) = NULL;
	void *fake[8] = {0};
	fake[1] = buf; /* yükleme nesnesinin +4'ü: dosya içeriği */
	*(void **)(tex + TEX_ASYNC) = fake;
	int r = ((TexProcess_t)ADDR_TEX_PROCESS)(tex);
	*(void **)(tex + TEX_ASYNC) = NULL;
	HeapFree(GetProcessHeap(), 0, buf);
	uint8_t *fresh = *(uint8_t **)(tex + TEX_GX);
	*(void **)(tex + TEX_GX) = oldGx;
	if (!r || !fresh) {
		/* işlenemedi: eski doku olduğu gibi kalsın */
		*(uint32_t *)(tex + TEX_FLAGS) = savedFlags;
		*(uint32_t *)(tex + TEX_GXFLAG) = savedGxFlags;
		return 0;
	}
	rebuild_gxtex_in_place(oldGx, fresh);
	((GxTexRelease_t)ADDR_GXTEX_RELEASE)(fresh);
	return 1;
}

/* Sahnedeki her karakterin modelini, oyunun görünüm değişikliğinde (şekil
 * değiştirme, binek, morph) kullandığı yolla baştan kurdurur: model dosyadan
 * güncel seçimle okunur, ten/saç/ekipman oyun tarafından yeniden uygulanır.
 * Oyunun kontrolü "görünüm değişti mi?" diye saklanan kaydı karşılaştırdığı
 * için, saklanan kaydın tek alanı farklı bir kopyası bir anlığına gösterilir;
 * yeniden kurma gerçek kaydı kendisi geri yazar. */
/* Is the object still in the object manager's list, with the given type?
 * Rebuilding one object can change others (the game may remove or replace
 * objects meanwhile), so every collected pointer is checked again right before
 * it is used; a stale or empty entry is skipped (it crashed at HDToggle+0x2998). */
static int scene_ready(void);
static int model_stale(uint8_t *model);
static const char *m2_name(uint8_t *sh);
static int g_skippedObjects;
static int object_alive(uint8_t *mgr, uint8_t *want, uint32_t typemask)
{
	uint8_t *m = *(uint8_t **)ADDR_OBJMGR;
	if (!want || !m || m != mgr)
		return 0;
	uintptr_t link = *(uintptr_t *)(m + OBJMGR_LINK);
	int guard = 0;
	for (uintptr_t o = *(uintptr_t *)(m + OBJMGR_FIRST); o && !(o & 1) && guard < 65536;
	     o = *(uintptr_t *)(o + link + 4), guard++) {
		if ((uint8_t *)o == want) {
			uint8_t *d = *(uint8_t **)(o + OBJ_DESC);
			return d && (*(uint32_t *)(d + 8) & typemask);
		}
	}
	return 0;
}

static int rebuild_units(void)
{
	uint8_t *mgr = *(uint8_t **)ADDR_OBJMGR;
	if (!mgr)
		return 0;
	static uint8_t *units[4096];
	int n = 0, done = 0;
	uintptr_t link = *(uintptr_t *)(mgr + OBJMGR_LINK);
	uintptr_t o = *(uintptr_t *)(mgr + OBJMGR_FIRST);
	while (o && !(o & 1) && n < 4096) {
		uint8_t *desc = *(uint8_t **)(o + OBJ_DESC);
		if (desc && (*(uint32_t *)(desc + 8) & TYPEMASK_UNIT))
			units[n++] = (uint8_t *)o;
		o = *(uintptr_t *)(o + link + 4);
	}
	static uint8_t fake[CDI_RECORD_SIZE];
	/* kameranın izlediği karakter (genellikle oyuncunun kendisi): dünya çerçevesi -> kamera -> +0x88 GUID */
	uint8_t *wf = *(uint8_t **)ADDR_WORLDFRAME, *cam = wf ? *(uint8_t **)(wf + WORLDFRAME_CAMERA) : NULL;
	uint64_t camGuid = cam ? *(uint64_t *)(cam + CAMERA_TARGET) : 0;
	for (int i = 0; i < n; i++) {
		uint8_t *u = units[i];
		if (!object_alive(mgr, u, TYPEMASK_UNIT) || !scene_ready()) {
			g_skippedObjects++;
			dbg1("character %p skipped: %s", (void *)u, scene_ready() ? "gone from the object list" : "scene not ready");
			continue;
		}
		uint8_t *real = *(uint8_t **)(u + UNIT_DISPLAYREC);
		uint8_t *desc = *(uint8_t **)(u + OBJ_DESC);
		uint64_t guid = desc ? *(uint64_t *)desc : 0;
		int me = camGuid && guid == camGuid;
		uint8_t *m0 = *(uint8_t **)(u + OBJ_MODEL), *c0 = *(uint8_t **)(u + UNIT_COMPONENT);
		char before[96] = "-";
		if (g_debug && m0 && *(uint8_t **)(m0 + MODEL_SHARED))
			snprintf(before, sizeof before, "%s%s", m2_name(*(uint8_t **)(m0 + MODEL_SHARED)),
			         model_stale(m0) ? " (stale)" : "");
		if (!real) {
			dbg1("character %08X%08X%s: no display record, not rebuilt (model %s)", (unsigned)(guid >> 32), (unsigned)guid,
			     me ? " [CAMERA/PLAYER]" : "", before);
			continue;
		}
		memcpy(fake, real, sizeof fake);
		*(uint32_t *)(fake + 0x0C) ^= 0x80000000u;
		*(uint8_t **)(u + UNIT_DISPLAYREC) = fake;
		((UnitRebuild_t)ADDR_UNIT_REBUILD)(u);
		int built = *(uint8_t **)(u + UNIT_DISPLAYREC) != fake;
		if (!built)
			*(uint8_t **)(u + UNIT_DISPLAYREC) = real; /* kurulmadı: eskisi kalsın */
		else
			done++;
		if (g_debug) {
			uint8_t *m1 = *(uint8_t **)(u + OBJ_MODEL), *c1 = *(uint8_t **)(u + UNIT_COMPONENT);
			char after[96] = "-";
			if (m1 && *(uint8_t **)(m1 + MODEL_SHARED))
				snprintf(after, sizeof after, "%s%s", m2_name(*(uint8_t **)(m1 + MODEL_SHARED)), model_stale(m1) ? " (stale)" : "");
			dbg1("character %08X%08X%s: %s; model %s -> %s (%s); skin component %p -> %p; display %u", (unsigned)(guid >> 32),
			     (unsigned)guid, me ? " [CAMERA/PLAYER]" : "", built ? "rebuilt" : "NOT rebuilt (game saw no change)", before,
			     after, m1 == m0 ? "same model object" : "new model object", (void *)c0, (void *)c1,
			     desc ? *(uint32_t *)(desc + UNIT_DESC_DISPLAYID) : 0);
		}
	}
	return done;
}

/* Modelin ortak verisi eskidi mi (dosyası değişti, adı işaretlendi)? */
static int model_stale(uint8_t *model)
{
	uint8_t *sh = model ? *(uint8_t **)(model + MODEL_SHARED) : NULL;
	return sh && sh[M2S_NAME] == M2_STALE_MARK;
}

/* Eskiyen modelin asıl adı (işaretten sonrası). */
static const char *m2_name(uint8_t *sh)
{
	const char *n = (const char *)(sh + M2S_NAME);
	return *n == M2_STALE_MARK ? n + 1 : n;
}

/* Haritadaki dekor objelerini (ağaç, fıçı, çit...) yeniden kurar: oyunun
 * dekor kaldırma adımları (geri çağırmaları temizle, modeli bırak) ve
 * CMapDoodadDef::CreateModel ile, aynı tanım ve konumla. */
/* The world scene can be used only while it is set up: a scene without its
 * model cache (+4) makes every model creation crash (0x706B8C, seen after a
 * world reload). */
#define SCENE_CACHE 0x04
static int scene_ready(void)
{
	uint8_t *scene = *(uint8_t **)ADDR_WORLD_SCENE;
	return scene && *(void **)(scene + SCENE_CACHE);
}

static int rebuild_doodads(void)
{
	uint8_t *scene = *(uint8_t **)ADDR_WORLD_SCENE;
	if (!scene_ready())
		return 0;
	static struct { uint8_t *def; char name[264]; } todo[8192];
	int n = 0, done = 0;
	for (uint8_t *m = *(uint8_t **)(scene + SCENE_MODELS); m && n < 8192; m = *(uint8_t **)(m + MODEL_NEXT)) {
		if (*(uintptr_t *)(m + MODEL_CB) != ADDR_DOODAD_CB || !model_stale(m))
			continue;
		uint8_t *def = *(uint8_t **)(m + MODEL_CBARG);
		if (!def || *(uint8_t **)(def + DOODAD_MODEL) != m)
			continue;
		const char *base = m2_name(*(uint8_t **)(m + MODEL_SHARED));
		todo[n].def = def;
		snprintf(todo[n].name, sizeof todo[n].name, "%s.m2", base);
		n++;
	}
	for (int i = 0; i < n; i++) {
		uint8_t *def = todo[i].def, *old = *(uint8_t **)(def + DOODAD_MODEL);
		if (!old)
			continue;
		((ModelSetCb_t)ADDR_MODEL_SETCB)(old, NULL, NULL, 0);
		((ModelSetLight_t)ADDR_MODEL_SETLIGHT)(old, NULL, NULL);
		((ModelRelease_t)ADDR_MODEL_RELEASE)(old);
		*(void **)(def + DOODAD_MODEL) = NULL;
		if (((DoodadCreate_t)ADDR_DOODAD_CREATE)(todo[i].name, def, 0))
			done++;
	}
	return done;
}

/* Oyun nesnelerini (sandık, kapı, posta kutusu...) yeniden kurar: oyunun
 * CGObject_C model oluşturma yolu (ad -> sahne modeli -> SetModel). */
static int rebuild_gameobjects(void)
{
	uint8_t *mgr = *(uint8_t **)ADDR_OBJMGR, *scene = *(uint8_t **)ADDR_WORLD_SCENE;
	if (!mgr || !scene_ready())
		return 0;
	static uint8_t *objs[4096];
	int n = 0, done = 0;
	uintptr_t link = *(uintptr_t *)(mgr + OBJMGR_LINK);
	for (uintptr_t o = *(uintptr_t *)(mgr + OBJMGR_FIRST); o && !(o & 1) && n < 4096; o = *(uintptr_t *)(o + link + 4)) {
		uint8_t *desc = *(uint8_t **)(o + OBJ_DESC);
		if (desc && (*(uint32_t *)(desc + 8) & TYPEMASK_GAMEOBJECT) && model_stale(*(uint8_t **)(o + OBJ_MODEL)))
			objs[n++] = (uint8_t *)o;
	}
	for (int i = 0; i < n; i++) {
		uint8_t *obj = objs[i];
		scene = *(uint8_t **)ADDR_WORLD_SCENE;
		if (!object_alive(mgr, obj, TYPEMASK_GAMEOBJECT) || !scene_ready() ||
		    !model_stale(*(uint8_t **)(obj + OBJ_MODEL))) {
			g_skippedObjects++;
			continue;
		}
		void **vt = *(void ***)obj;
		const char *name = ((ObjModelName_t)vt[OBJ_VT_MODELNAME])(obj);
		if (!name || !name[0])
			continue;
		void *model = ((SceneCreate_t)ADDR_SCENE_CREATE)(scene, name, 0);
		if (!model)
			continue;
		((ObjSetModel_t)ADDR_OBJ_SETMODEL)(obj, model);
		((ModelRelease_t)ADDR_MODEL_RELEASE)(model); /* SetModel kendi referansını aldı */
		done++;
	}
	return done;
}

/* Dosyası değişen bina verilerini (CMapObj) önbellekten çıkarır. Önbellek
 * yalnızca hash'i karşılaştırdığı için hash, o kovaya hiçbir aramanın
 * düşemeyeceği bir değere çevrilir; nesne kendi yaşam döngüsünde silinir. */
static int evict_mapobjs(LONG changed)
{
	uint8_t *buckets = *(uint8_t **)ADDR_MAPOBJ_BUCKETS;
	uint32_t mask = *(uint32_t *)ADDR_MAPOBJ_MASK;
	if (!buckets || mask == 0xFFFFFFFFu)
		return 0;
	int n = 0;
	char root[300], group[300];
	for (uint32_t b = 0; b <= mask; b++) {
		uint8_t *bucket = buckets + b * 12;
		uint32_t linkoff = *(uint32_t *)bucket;
		for (uintptr_t o = *(uintptr_t *)(bucket + 8); o && !(o & 1);) {
			uintptr_t next = *(uintptr_t *)(o + linkoff + 4);
			const char *name = (const char *)(o + MAPOBJ_NAME);
			snprintf(root, sizeof root, "%s", name);
			snprintf(group, sizeof group, "%s", name);
			char *dot = strrchr(group, '.');
			if (dot && (size_t)(dot - group) + 9 < sizeof group)
				strcpy(dot, "_000.wmo");
			int stale = 0;
			for (int k = 0; k < 2 && !stale; k++) {
				const char *f = k ? group : root;
				stale = touched_by(f, changed) && effective_owner(f, g_appliedMask) != effective_owner(f, g_offMask);
			}
			if (stale) {
				uint32_t h = *(uint32_t *)(o + MAPOBJ_HASH);
				*(uint32_t *)(o + MAPOBJ_HASH) = (h & ~mask) | ((b + 1) & mask);
				n++;
			}
			o = next;
		}
	}
	return n;
}

/* Bir tablonun bellekteki kayıt boyutu: ana bloktaki (+0, +4 kayıt) kayıtlar
 * ardışık durduğundan dizinin gösterdiği en küçük pozitif uzaklık; hepsi bunun
 * katı değilse 0. Yalnızca hiç değiştirilmemiş tablolarda ölçülür (oyunun ilk
 * yüklediği canlı tablo, saklanan sürümler). */
static uint32_t db_stride(const uint8_t *db)
{
	uint8_t *base = *(uint8_t **)(db + DB_RECORDS), **index = *(uint8_t ***)(db + DB_INDEX);
	uint32_t maxid = *(uint32_t *)(db + DB_MAXID), count = *(uint32_t *)(db + DB_COUNT), stride = 0;
	if (!base || !index || count < 2)
		return 0;
	for (uint32_t i = 0; i <= maxid; i++)
		if (index[i] > base && (!stride || (uint32_t)(index[i] - base) < stride))
			stride = (uint32_t)(index[i] - base);
	for (uint32_t i = 0; stride && i <= maxid; i++)
		if (index[i] && (index[i] < base || ((uint32_t)(index[i] - base) % stride) ||
		                 (uint32_t)(index[i] - base) / stride >= count))
			stride = 0;
	return stride;
}

/* Her tablonun her dosya sürümü (dosyanın okunduğu arşiv) bir kez okunur ve
 * saklanır: sürüm sayısı sınırlı olduğundan bellek, kaç kez aç/kapat yapılırsa
 * yapılsın sabit kalır. Saklanan sürümler hiç değiştirilmez (kayıtlar canlı
 * tabloya kopyalanır), metinleri canlı kayıtlarca gösterildiği için silinmez. */
typedef struct { const DbInfo *db; void *owner; uint8_t tab[0x20]; uint32_t stride; } DbVersion;
#define DB_VERSIONS_MAX 96
static DbVersion g_dbVersions[DB_VERSIONS_MAX];
static int g_dbVersionCount;
static uint32_t g_dbLiveStride[NDBS]; /* canlı tablonun kayıt boyutu: ilk yeniden yüklemede, el değmemişken ölçülür */

static DbVersion *db_version(const DbInfo *db, void *owner)
{
	for (int i = 0; i < g_dbVersionCount; i++)
		if (g_dbVersions[i].db == db && g_dbVersions[i].owner == owner)
			return &g_dbVersions[i];
	if (g_dbVersionCount == DB_VERSIONS_MAX)
		return NULL;
	DbVersion *v = &g_dbVersions[g_dbVersionCount];
	memset(v, 0, sizeof *v);
	((DbLoad_t)db->load)(v->tab);
	if (!*(uint32_t *)(v->tab + DB_LOADED) || !*(void **)(v->tab + DB_INDEX))
		return NULL; /* yüklenemedi (bu durumda oyun da hata verirdi) */
	v->db = db, v->owner = owner, v->stride = db_stride(v->tab);
	g_dbVersionCount++;
	return v;
}

/* Değişen paketlerin etkilediği DBC tablolarını yerinde yeniden yükler: dosyanın
 * güncel sürümünün kayıtları kimliğe göre canlı tablonun aynı adreslerine
 * kopyalanır; kayıt işaretçisi saklayan her yer geçerli kalır. Canlı tabloda
 * olmayan kimlikler (ör. C'nin yeni modelleri) ayrı belleğe kopyalanıp eklenir:
 * eklenmezlerse onlara başvuran kayıtlar boş kayda çıkar ve oyun çöker
 * (0x60B0A9). Yeni dosyada olmayan kimlikler eski içerikle kalır (onlara
 * başvuran eski veri hâlâ geçerli bir kayıt bulur). */
static int reload_dbcs(LONG changed, LONG now)
{
	int tables = 0;
	for (size_t k = 0; k < NDBS; k++) {
		const DbInfo *db = &DBS[k];
		uint8_t *live = (uint8_t *)db->table;
		if (!*(uint32_t *)(live + DB_LOADED) || !touched_by(db->file, changed))
			continue;
		void *owner = effective_owner(db->file, now);
		if (owner == effective_owner(db->file, g_appliedMask))
			continue;
		if (!g_dbLiveStride[k])
			g_dbLiveStride[k] = db_stride(live);
		uint32_t ls = g_dbLiveStride[k];
		DbVersion *v = owner ? db_version(db, owner) : NULL;
		if (!v || !ls || v->stride != ls) {
			hdlog("DBC skipped: %s (record size %u/%u)", db->file, ls, v ? v->stride : 0);
			continue;
		}
		uint8_t **lidx = *(uint8_t ***)(live + DB_INDEX), **tidx = *(uint8_t ***)(v->tab + DB_INDEX);
		uint32_t lmax = *(uint32_t *)(live + DB_MAXID), tmax = *(uint32_t *)(v->tab + DB_MAXID), copied = 0, added = 0;
		int grow = tmax > lmax;
		for (uint32_t id = 0; !grow && id <= tmax; id++)
			grow = tidx[id] && !lidx[id];
		uint32_t nmax = tmax > lmax ? tmax : lmax;
		uint8_t **nidx = lidx;
		if (grow) {
			/* eski dizin, o an okuyan olabileceği için bırakılmaz (küçük, tablo başına bir kez büyür) */
			nidx = ((SMemAlloc_t)ADDR_SMEM_ALLOC)((nmax + 1) * sizeof *nidx, "HDToggle", __LINE__, 0);
			if (!nidx) {
				hdlog("DBC skipped: %s (out of memory)", db->file);
				continue;
			}
			memset(nidx, 0, (nmax + 1) * sizeof *nidx);
			memcpy(nidx, lidx, (lmax + 1) * sizeof *nidx);
		}
		for (uint32_t id = 0; id <= tmax; id++) {
			if (!tidx[id])
				continue;
			if (!nidx[id]) {
				uint8_t *rec = ((SMemAlloc_t)ADDR_SMEM_ALLOC)(ls, "HDToggle", __LINE__, 0);
				if (!rec)
					continue;
				memcpy(rec, tidx[id], ls);
				nidx[id] = rec;
				added++;
			} else {
				memcpy(nidx[id], tidx[id], ls); /* aynı adres: saklanan işaretçiler geçerli */
				copied++;
			}
		}
		if (nidx != lidx) {
			*(uint8_t **volatile *)(live + DB_INDEX) = nidx; /* önce dizin, sonra sınır: okuyan taşmaz */
			*(volatile uint32_t *)(live + DB_MAXID) = nmax;
		}
		hdlog("DBC reloaded: %s (%u records, %u new)", db->file, copied, added);
		tables++;
	}
	return tables;
}

/* Dosyayı güncel seçimle belleğe okur (SFile, gevşek dosya + MPQ). */
static uint8_t *read_whole(const char *name, int flag, uint32_t *size)
{
	void *h = NULL;
	if (!((SFile2Open_t)ADDR_SFILE2_OPEN)(0, name, flag, &h) || !h)
		return NULL;
	uint32_t hi = 0, sz = ((SFile2Size_t)ADDR_SFILE2_SIZE)(h, &hi), got = 0;
	uint8_t *buf = sz && !hi ? HeapAlloc(GetProcessHeap(), 0, sz) : NULL;
	int ok = buf && ((SFile2Read_t)ADDR_SFILE2_READ)(h, buf, sz, &got, 0, 0) && got == sz;
	((SFile2Close_t)ADDR_SFILE2_CLOSE)(h);
	if (!ok && buf) {
		HeapFree(GetProcessHeap(), 0, buf);
		buf = NULL;
	}
	*size = sz;
	return buf;
}

/* Çözülmüş hali bellekte tutulan dokuyu (karakter ten/yüz/saç parçaları gibi)
 * yeni dosyasından yeniden çözer. Oyunun çözme fonksiyonu yeni görüntüyü
 * ayırıp boyutları yazar; eski görüntü ancak bu başarılı olursa bırakılır. */
/* Çözücü (0x449840) +0x118'i istek olarak okur ve otomatik isteklerin yerine seçtiği
 * biçimi yazar: istek 8 -> alfa yoksa 5, 1 bit alfa 3, fazlası 4; istek 9 -> alfa yoksa 5,
 * varsa 9. Karakter dokusu önbelleği (0x47AE30) parçaları 9 (bazılarını 5) ile ister.
 * Yeni dosyanın alfası eskisinden farklı olabileceği için seçilmiş biçimler isteğe
 * döndürülür; yoksa alfasız parçalar (tenin kendisi) eski HD görüntüde kalıyordu:
 * paketler kapanınca 1024'lük ten OctoWoW'un modeline giydiriliyor, yüz bozuluyordu. */
static int image_request(int fmt)
{
	return fmt == 5 ? 9 : fmt == 3 || fmt == 4 ? 8 : fmt;
}

static int reload_image_texture(uint8_t *tex)
{
	uint32_t size;
	uint8_t *buf = read_whole((const char *)(tex + TEX_NAME), (*(uint32_t *)(tex + TEX_FLAGS) & 2) != 0, &size);
	if (!buf)
		return 0;
	void *oldBits = *(void **)(tex + TEX_BITS);
	int oldFmt = *(int *)(tex + TEX_FORMAT), oldW = *(int *)(tex + TEX_W), oldH = *(int *)(tex + TEX_H);
	uint32_t savedFlags = *(uint32_t *)(tex + TEX_FLAGS);
	*(uint32_t *)(tex + TEX_FLAGS) &= ~1u;
	*(void **)(tex + TEX_BITS) = NULL;
	*(int *)(tex + TEX_FORMAT) = image_request(oldFmt);
	void *fake[8] = {0};
	fake[1] = buf;
	*(void **)(tex + TEX_ASYNC) = fake;
	int r = ((TexProcess_t)ADDR_TEX_DECODE_IMAGE)(tex);
	*(void **)(tex + TEX_ASYNC) = NULL;
	HeapFree(GetProcessHeap(), 0, buf);
	if (!r || !*(void **)(tex + TEX_BITS)) {
		/* çözülemedi: eski görüntü olduğu gibi kalsın */
		*(void **)(tex + TEX_BITS) = oldBits;
		*(int *)(tex + TEX_FORMAT) = oldFmt, *(int *)(tex + TEX_W) = oldW, *(int *)(tex + TEX_H) = oldH;
		*(uint32_t *)(tex + TEX_FLAGS) = savedFlags;
		return 0;
	}
	if (oldBits)
		((TexFreeImage_t)ADDR_TEX_FREE_IMAGE)(oldBits, oldFmt, oldW, oldH);
	return 1;
}

static int patch_bytes(uintptr_t addr, const void *bytes, size_t len);

static int scale_of(uint32_t v) { return v == 256 ? 0 : v == 512 ? 1 : v == 1024 ? 2 : -1; }

/* Ten birleştirme yerleşimini istenen ölçeğe getirir (en büyük ölçeği aşmaz). */
static int set_skin_layout(int want)
{
	if (g_vhMax < 0)
		return -1;
	if (want > g_vhMax)
		want = g_vhMax;
	int cur = scale_of(*(uint32_t *)VH_LAYOUT_256[1]);
	if (cur < 0 || cur == want)
		return cur;
	uint32_t v;
	for (size_t i = 0; i < sizeof VH_LAYOUT_256 / sizeof VH_LAYOUT_256[0]; i++)
		v = 256u << want, patch_bytes(VH_LAYOUT_256[i], &v, 4);
	for (size_t i = 0; i < sizeof VH_LAYOUT_512 / sizeof VH_LAYOUT_512[0]; i++)
		v = 512u << want, patch_bytes(VH_LAYOUT_512[i], &v, 4);
	for (size_t i = 0; i < sizeof VH_LAYOUT_255 / sizeof VH_LAYOUT_255[0]; i++)
		v = (256u << want) - 1, patch_bytes(VH_LAYOUT_255[i], &v, 4);
	uint32_t table[VH_REGION_COUNT];
	memcpy(table, (void *)VH_REGION_TABLE, sizeof table);
	for (int i = 0; i < VH_REGION_COUNT; i++)
		table[i] = want > cur ? table[i] << (want - cur) : table[i] >> (cur - want);
	DWORD old;
	if (VirtualProtect((void *)VH_REGION_TABLE, sizeof table, PAGE_READWRITE, &old)) {
		memcpy((void *)VH_REGION_TABLE, table, sizeof table);
		VirtualProtect((void *)VH_REGION_TABLE, sizeof table, old, &old);
	}
	hdlog("character skin layout %d -> %d (buffer scale %d)", cur, want, g_vhMax);
	return want;
}

/* Yerleşim: açık paketlerin en büyük ten ölçeği (hiçbiri yoksa vanilla). OctoWoW
 * dışında VanillaHelpers'ın kendi seçimi kalır. */
static int wanted_skin_scale(LONG mask)
{
	if (g_locked)
		return g_vhMarker < 0 ? 0 : g_vhMarker;
	int want = 0;
	for (int b = 0; b < 26; b++)
		if (g_packSkin[b] > want && !(mask & (1L << b)))
			want = g_packSkin[b];
	return want;
}

/* --- ten ölçeği ölçümü ------------------------------------------------------
 * Paketin temel ten dokusu (Character\Irk\Cinsiyet\IrkCinsiyetSkin00_NN.blp,
 * vanilla'da 256x256) bu arşivde varsa genişliğinden ölçek çıkar: 512 -> 1 (2x),
 * 1024 -> 2 (4x). Arşive özel arama kancadan geçmez; kapalı paketler de ölçülür. */
static const char *const SKIN_RACES[] = {"Human", "Orc", "Dwarf", "NightElf", "Scourge", "Tauren",
                                         "Gnome", "Troll", "Goblin", "BloodElf"};
static const char *const SKIN_SEXES[] = {"Male", "Female"};
#define SKIN_COLORS 20

static int blp_size_in(void *arc, const char *name, uint32_t *w, uint32_t *h)
{
	void *fh = NULL;
	if (!((SFileOpen_t)ADDR_SFILE_OPEN)(arc, name, 0, &fh) || !fh)
		return 0;
	uint8_t head[20];
	uint32_t got = 0;
	int ok = ((SFileRead_t)ADDR_SFILE_READ)(fh, head, sizeof head, &got, NULL, 0, 0) && got == sizeof head &&
	         memcmp(head, "BLP2", 4) == 0;
	((SFileClose_t)ADDR_SFILE_CLOSE)(fh);
	if (ok)
		*w = *(uint32_t *)(head + 12), *h = *(uint32_t *)(head + 16);
	return ok;
}

static int skin_scale_of_pack(void *arc)
{
	int scale = -1;
	char name[96];
	for (size_t r = 0; r < sizeof SKIN_RACES / sizeof SKIN_RACES[0]; r++)
		for (size_t x = 0; x < 2; x++)
			for (int c = 0; c < SKIN_COLORS; c++) {
				snprintf(name, sizeof name, "Character\\%s\\%s\\%s%sSkin00_%02d.blp", SKIN_RACES[r], SKIN_SEXES[x],
				         SKIN_RACES[r], SKIN_SEXES[x], c);
				if (archive_lookup(arc, name) != 1)
					continue;
				uint32_t w = 0, h = 0;
				if (blp_size_in(arc, name, &w, &h) && w == h) {
					int s = scale_of(w);
					if (s > scale)
						scale = s;
				}
				break; /* bu ırk/cinsiyet için bir doku yeter */
			}
	return scale;
}

static void measure_pack_skins(void)
{
	ArcInfo list[64];
	int n = snapshot(list, 64);
	g_skinPacks = 0;
	for (int i = 0; i < n; i++) {
		int b = list[i].bit;
		if (b < 0)
			continue;
		g_packSkin[b] = (signed char)skin_scale_of_pack(list[i].arc);
		if (g_packSkin[b] >= 0) {
			g_skinPacks |= 1L << b;
			hdlog("pack %c: character skins %dx", 'A' + b, 1 << g_packSkin[b]);
		}
	}
}

/* --- ten parçasını bölgeye uydurma ------------------------------------------
 * Oyun her parçayı (0x477460 -> 0x477C80/0x477DB0/0x477F20) ölçeklemeden
 * kopyalar: bölge boyutu kadar pikseli, kaynağın kendi mip sayısı kadar
 * seviyede. Bölgeden büyük doku (4x doku, 2x yerleşim) tamponun son
 * seviyesinden sonrasına yazar (0x477D15'te çökme); küçük doku
 * (1x doku, 2x yerleşim) VanillaHelpers'ın sınır denetimine takılıp hiç
 * çizilmez. Doku bölgeden 2^k büyükse ilk k mip seviyesi atlanır; küçükse
 * eksik üst seviyeler en yakın komşu ile büyütülerek üretilir. Alfa maskesi
 * (piksel başına 2 bit, seviyenin piksellerinin hemen ardında) aynı şekilde
 * büyütülür. Seviye sayısı yerleşimin seviye sayısını hiç aşmaz. */
#define ADDR_SKIN_PASTE 0x00477460u /* thiscall(bileşen, mipler, alfa, hedef*, kaynak*, boyut*, bilgi*), ret 0x18 */
static const uintptr_t SKIN_PASTE_CALLS[] = {0x004770D7u, 0x0047715Bu};
#define PASTE_LEVELS_MAX 16
typedef void (__thiscall *SkinPaste_t)(void *comp, uint8_t **mips, int alpha, int32_t *dst, int32_t *src,
                                         int32_t *size, uint32_t *info);
static SkinPaste_t g_skinPaste;
static int g_pasteFitted, g_pasteSkipped;

/* a = b * 2^k ise k, değilse 99 */
static int pow2_ratio(uint32_t a, uint32_t b)
{
	if (!a || !b)
		return 99;
	for (int k = 0; k <= 4; k++) {
		if (a == b << k)
			return k;
		if (b == a << k)
			return -k;
	}
	return 99;
}

static void upscale_level(const uint8_t *src, uint32_t sw, uint32_t sh, int withMask, uint8_t *out, int shift)
{
	uint32_t w = sw << shift, h = sh << shift;
	const uint16_t *sp = (const uint16_t *)src;
	uint16_t *op = (uint16_t *)out;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++)
			op[y * w + x] = sp[(y >> shift) * sw + (x >> shift)];
	if (!withMask)
		return;
	const uint8_t *sm = src + sw * sh * 2;
	uint8_t *om = out + w * h * 2;
	memset(om, 0, (w * h + 3) / 4);
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t si = (y >> shift) * sw + (x >> shift), oi = y * w + x;
			om[oi >> 2] |= (uint8_t)(((sm[si >> 2] >> ((si & 3) * 2)) & 3) << ((oi & 3) * 2));
		}
}

/* debug: birleştirilen parçanın doku adı (oyunun doku listesinde çözülmüş verisi bu olan doku) */
static const char *image_name(uint8_t **mips)
{
	uintptr_t link = *(uintptr_t *)ADDR_TEX_LIST_LINK;
	uintptr_t t = *(uintptr_t *)ADDR_TEX_LIST_HEAD;
	for (int guard = 0; t && !(t & 1) && guard < 65536; t = *(uintptr_t *)(t + link + 4), guard++) {
		void *bits = *(void **)(t + TEX_BITS);
		if (bits && mips && (bits == (void *)mips || bits == (void *)mips[0]))
			return (const char *)(t + TEX_NAME);
	}
	return "?";
}

static void __fastcall hook_skin_paste(void *comp, void *edx, uint8_t **mips, int alpha, int32_t *dst, int32_t *src,
                                       int32_t *size, uint32_t *info)
{
	(void)edx;
	int layout = scale_of(*(uint32_t *)VH_LAYOUT_256[1]);
	if (!mips || !info || !size || layout < 0 || !info[0] || !info[1]) {
		g_skinPaste(comp, mips, alpha, dst, src, size, info);
		return;
	}
	uint32_t levels = 9 + (uint32_t)layout; /* 256<<yerleşim dokusunun seviye sayısı */
	/* temel ten (kaynak konumu = hedef konumu) bütün tuvalin boyutundadır */
	uint32_t ew = src == dst ? 256u << layout : (uint32_t)size[0];
	uint32_t eh = src == dst ? 256u << layout : (uint32_t)size[1];
	uint32_t sw = info[0], sh = info[1];
	int k = pow2_ratio(sw, ew);
	uint32_t fit[6];
	memcpy(fit, info, sizeof fit);
	dbg2("skin paste comp %p: %s %ux%u (%u mips), region %ux%u at %d,%d, layout %d, %s", comp,
	     g_debug >= 2 ? image_name(mips) : "", sw, sh, info[3], ew, eh, dst ? dst[0] : -1, dst ? dst[1] : -1, layout,
	     k == 0 ? "same size" : k == 99 || k != pow2_ratio(sh, eh) ? "not a power-of-two ratio" : k > 0 ? "downscaled" : "upscaled");
	if (g_locked || k != pow2_ratio(sh, eh) || k == 99) {
		/* yalnız güvenlik: tamponun dışına yazma */
		if (fit[3] > levels)
			fit[3] = levels;
		if (!g_locked && (sw < ew || sh < eh)) {
			if (g_pasteSkipped++ < 16 || g_debug)
				hdlog("skin part %ux%u does not fit region %ux%u: skipped%s%s", sw, sh, ew, eh, g_debug ? " " : "",
				      g_debug ? image_name(mips) : "");
			return;
		}
		g_skinPaste(comp, mips, alpha, dst, src, size, fit);
		return;
	}
	if (k == 0) {
		if (fit[3] > levels)
			fit[3] = levels;
		g_skinPaste(comp, mips, alpha, dst, src, size, fit);
		return;
	}
	uint8_t *m[PASTE_LEVELS_MAX];
	uint8_t *block = NULL;
	uint32_t count;
	if (k > 0) {
		if (info[3] <= (uint32_t)k)
			return; /* bölge boyutunda seviyesi yok */
		count = info[3] - (uint32_t)k;
		if (count > PASTE_LEVELS_MAX)
			count = PASTE_LEVELS_MAX;
		for (uint32_t i = 0; i < count; i++)
			m[i] = mips[i + (uint32_t)k];
		fit[0] = sw >> k, fit[1] = sh >> k;
	} else {
		int u = -k;
		size_t bytes = 0, at[4];
		for (int j = 0; j < u; j++) {
			size_t px = (size_t)(sw << (u - j)) * (sh << (u - j));
			at[j] = bytes;
			bytes += px * 2 + (px + 3) / 4 + 4;
		}
		block = malloc(bytes);
		if (!block)
			return;
		for (int j = 0; j < u; j++) {
			m[j] = block + at[j];
			upscale_level(mips[0], sw, sh, alpha != 0, m[j], u - j);
		}
		count = (uint32_t)u + info[3];
		if (count > PASTE_LEVELS_MAX)
			count = PASTE_LEVELS_MAX;
		for (uint32_t i = (uint32_t)u; i < count; i++)
			m[i] = mips[i - (uint32_t)u];
		fit[0] = sw << u, fit[1] = sh << u;
	}
	if (count > levels)
		count = levels;
	fit[3] = count;
	if (g_pasteFitted++ < 16 || g_debug)
		hdlog("skin part %ux%u fitted to region %ux%u%s%s", sw, sh, ew, eh, g_debug ? " " : "", g_debug ? image_name(mips) : "");
	g_skinPaste(comp, m, alpha, dst, src, size, fit);
	free(block);
}

/* Cesetlerin ten dokusunu bırakır; bir sonraki hazırlıkta yeni ölçekle oluşur. */
static int reset_corpse_skins(void)
{
	uint8_t *mgr = *(uint8_t **)ADDR_OBJMGR;
	if (!mgr)
		return 0;
	int n = 0;
	uintptr_t link = *(uintptr_t *)(mgr + OBJMGR_LINK);
	for (uintptr_t o = *(uintptr_t *)(mgr + OBJMGR_FIRST); o && !(o & 1); o = *(uintptr_t *)(o + link + 4)) {
		uint8_t *desc = *(uint8_t **)(o + OBJ_DESC);
		uint8_t *comp = desc && (*(uint32_t *)(desc + 8) & TYPEMASK_CORPSE) ? *(uint8_t **)(o + CORPSE_COMPONENT) : NULL;
		if (comp && *(void **)(comp + COMPONENT_TEXTURE)) {
			((HandleClose_t)ADDR_HANDLE_CLOSE)(*(void **)(comp + COMPONENT_TEXTURE));
			*(void **)(comp + COMPONENT_TEXTURE) = NULL;
			comp[COMPONENT_DIRTY] = 1;
			n++;
		}
	}
	return n;
}

/* HDPack_Refresh() -> yeniden kurulan doku sayısı | nil
 * Son yenilemeden bu yana açılıp kapanan paketlerden etkilenen ve ekranda
 * kullanılan dokuları yeni boyutlarıyla baştan kurar. */
static int refresh_core_body(int glue);
static int refresh_core(int glue)
{
	static int busy;
	if (busy) {
		hdlog("refresh already running: nested call ignored");
		return 0;
	}
	busy = 1;
	g_skippedObjects = 0;
	int r = refresh_core_body(glue);
	busy = 0;
	return r;
}

static int refresh_core_body(int glue)
{
	LONG now = g_wantMask, changed = now ^ g_appliedMask;
	LONG before = g_appliedMask;
	InterlockedExchange(&g_offMask, now); /* bundan sonra her dosya yeni seçimle okunur */
	int reloaded = 0, failed = 0, seen = 0, other = 0, images = 0;
	DWORD t0 = GetTickCount();
	dbg_reset_budget();
	memlog("before refresh");
	if (g_debug) {
		char a[27], b[27], c[27];
		mask_to_letters(before, a), mask_to_letters(now, b), mask_to_letters(changed, c);
		hdlog("D1 refresh start (%s): off before [%s], off now [%s], changed [%s]; skin layout %d, buffer %d",
		      glue ? "glue" : "world", a, b, c, scale_of(*(uint32_t *)VH_LAYOUT_256[1]), g_vhMax);
	}
	if (changed) {
		/* önce listeyi topla (yeniden kurma listeyi değiştirmez, yine de güvenli) */
		static uint8_t *texs[65536];
		int n = 0;
		uintptr_t link = *(uintptr_t *)ADDR_TEX_LIST_LINK;
		uintptr_t t = *(uintptr_t *)ADDR_TEX_LIST_HEAD;
		while (t && !(t & 1) && n < 65536) {
			texs[n++] = (uint8_t *)t;
			t = *(uintptr_t *)(t + link + 4);
		}
		for (int i = 0; i < n; i++) {
			uint8_t *tex = texs[i];
			const char *name = (const char *)(tex + TEX_NAME);
			uint8_t *gx = *(uint8_t **)(tex + TEX_GX);
			if (*(void **)(tex + TEX_ASYNC) || !name[0])
				continue; /* hâlâ yükleniyor ya da adsız: zaten güncel seçimle kurulacak */
			if (!gx) {
				/* Bellekte çözülmüş görüntü (karakter parçaları: ten, yüz, saç...). Biçimi
				 * çözücünün seçtiği (3/4/5) olanlar da yeniden çözülür; istek
				 * reload_image_texture'da oyunun asıl isteğine döndürülür. */
				if (*(void **)(tex + TEX_BITS) && touched_by(name, changed)) {
					void *was = effective_owner(name, g_appliedMask), *will = effective_owner(name, now);
					if (was != will) {
						int fmt0 = *(int *)(tex + TEX_FORMAT), w0 = *(int *)(tex + TEX_W), h0 = *(int *)(tex + TEX_H);
						int ok = reload_image_texture(tex);
						ok ? images++ : failed++;
						dbg1("image %s %s: %s -> %s, %dx%d fmt %d -> %dx%d fmt %d", ok ? "re-decoded" : "FAILED", name,
						     arc_label(was), arc_label(will), w0, h0, fmt0, *(int *)(tex + TEX_W), *(int *)(tex + TEX_H),
						     *(int *)(tex + TEX_FORMAT));
					} else {
						dbg2("image unchanged %s (still %s)", name, arc_label(was));
					}
				} else if (*(void **)(tex + TEX_BITS)) {
					dbg2("image not touched %s", name);
				} else {
					dbg2("image without bits skipped %s", name);
				}
				continue;
			}
			/* Yalnızca dosyadan akan standart BLP dokuları. Karakter/saç gibi
			 * çözülmüş veriyi bellekte tutan, kendi geri çağırması olan dokular
			 * bu yolla kurulamaz; onlar yeniden birleştirilince güncellenir. */
			if (*(uintptr_t *)(gx + GXTEX_USERFUNC) != ADDR_TEX_BLP_CALLBACK ||
			    *(uint8_t **)(gx + GXTEX_USERARG) != tex) {
				other++;
				if (touched_by(name, changed))
					dbg1("texture with its own callback (0x%08X) not rebuilt here: %s", (unsigned)*(uintptr_t *)(gx + GXTEX_USERFUNC), name);
				continue;
			}
			seen++;
			if (!touched_by(name, changed))
				continue;
			void *was = effective_owner(name, g_appliedMask), *will = effective_owner(name, now);
			if (was == will) {
				dbg2("texture unchanged %s (still %s)", name, arc_label(was));
				continue;
			}
			int ok = reload_texture(tex);
			ok ? reloaded++ : failed++;
			dbg1("texture %s %s: %s -> %s, now %ux%u", ok ? "rebuilt" : "FAILED", name, arc_label(was), arc_label(will),
			     *(uint32_t *)(gx + GXTEX_DESC), *(uint32_t *)(gx + GXTEX_DESC + 4));
		}
	}
	/* Etkilenen modelleri eskimiş işaretle: sonraki her yükleme dosyayı güncel
	 * seçimle okur; eskiler oyunun kendi yaşam döngüsünde silinir. */
	int models = 0;
	if (changed) {
		char file[300];
		for (int b = 0; b < M2CACHE_NBUCKETS; b++) {
			for (uint8_t *sh = *(uint8_t **)(ADDR_M2CACHE_BUCKETS + b * 4); sh; sh = *(uint8_t **)(sh + M2S_NEXT)) {
				char *name = (char *)(sh + M2S_NAME);
				size_t len = strnlen(name, M2S_NAMELEN);
				if (name[0] == M2_STALE_MARK || len + 2 > M2S_NAMELEN)
					continue;
				snprintf(file, sizeof file, "%s.m2", name);
				if (touched_by(file, changed) && effective_owner(file, g_appliedMask) != effective_owner(file, now)) {
					dbg1("model marked stale %s: %s -> %s", file, arc_label(effective_owner(file, g_appliedMask)),
					     arc_label(effective_owner(file, now)));
					memmove(name + 1, name, len + 1);
					name[0] = M2_STALE_MARK;
					models++;
				}
			}
		}
	}
	if (models)
		InterlockedExchange(&g_staleUntil, (LONG)(GetTickCount() + 60000));
	/* Bina verisini önbellekten çıkar, sonra dünyayı oyunun ışınlanma yoluyla
	 * baştan yükle: arazi, binalar ve dekorlar sonraki karelerde yeni
	 * dosyalardan kurulur. Dünya yüklü değilse dekorlar tek tek kurulur. */
	/* Arayüz (I), harita/yükleme ekranı (M) ve ses (S) dünyadaki hiçbir şeyi
	 * etkilemez: yalnız bunlar değiştiyse dünya ve karakterler yeniden kurulmaz. */
	LONG worldChanged = changed & ~((1L << ('I' - 'A')) | (1L << ('M' - 'A')) | (1L << ('S' - 'A')));
	if (glue)
		worldChanged = 0; /* character select / create: there is no world, units or game objects */
	DWORD tTex = GetTickCount();
	int dbcs = changed ? reload_dbcs(changed, now) : 0;
	int layoutBefore = scale_of(*(uint32_t *)VH_LAYOUT_256[1]);
	set_skin_layout(wanted_skin_scale(now));
	if ((changed & g_skinPacks) || scale_of(*(uint32_t *)VH_LAYOUT_256[1]) != layoutBefore)
		reset_corpse_skins();
	int wmos = 0, world = 0, doodads = 0, units = 0, gobjs = 0;
	if (worldChanged) {
		/* characters and game objects first, while the world scene is surely
		 * set up; the world reload below comes after */
		if (scene_ready()) {
			units = rebuild_units();
			gobjs = rebuild_gameobjects();
		} else if (*(void **)ADDR_WORLD_SCENE) {
			hdlog("world scene not ready: characters and game objects not rebuilt");
		}
		wmos = evict_mapobjs(changed);
		uintptr_t areas = *(uintptr_t *)ADDR_MAP_AREAS;
		if (areas && !(areas & 1)) {
			((MapUnloadAll_t)ADDR_MAP_UNLOAD_ALL)();
			*(uint32_t *)ADDR_WORLD_SNAP = 1;
			world = 1;
		} else {
			doodads = rebuild_doodads();
		}
	}
	g_appliedMask = now;
	if (g_debug)
		hdlog("D1 refresh timing: textures+models %lu ms, DBC+world+characters %lu ms; detail lines left %ld/%ld",
		      (unsigned long)(tTex - t0), (unsigned long)(GetTickCount() - tTex), (long)g_dbgBudget1, (long)g_dbgBudget2);
	hdlog("refresh: %d BLP textures scanned, %d rebuilt, %d images re-decoded, %d failed, %d other textures skipped; %d models, %d buildings evicted; "
	      "%d DBC; world %s; %d characters, %d doodads, %d game objects rebuilt",
	      seen, reloaded, images, failed, other, models, wmos, dbcs, world ? "reloading" : "not loaded", units, doodads, gobjs);
	if (g_skippedObjects)
		hdlog("refresh: %d characters/objects skipped (gone or changed during the rebuild)", g_skippedObjects);
	memlog("after refresh");
	return reloaded;
}

/* Checked on every API call. Moving to a listed server (its realm list, or one of
 * its realms behind a proxy) locks the DLL and asks for every pack to be
 * turned back on. The packs are restored by the next HDPack_Refresh, which the
 * addon schedules at a safe moment (never while the world is loading). */
static int enforce_server(void)
{
	if (g_locked)
		return 0;
	if (server_ok_now())
		return 1;
	InterlockedExchange(&g_locked, 1);
	InterlockedExchange(&g_wantMask, 0);
	hdlog("server not supported: HD Switch locked, all packs will be restored");
	return 0;
}

static int __fastcall lua_HDPack_Refresh(void *L)
{
	if (!g_hooksOk || !enforce_server()) {
		/* locked: only the one restoring refresh (back to all packs on) */
		if (g_hooksOk && g_locked && (g_appliedMask || g_offMask))
			refresh_core(0);
		lua_pushnil(L);
		return 1;
	}
	lua_pushnumber(L, refresh_core(0));
	return 1;
}

/* --- character select / create (glue screens) --------------------------------
 * Addons are not loaded there, so the DLL adds the button itself: right after
 * the game registers the glue script functions (call site 0x46A880), the HDPack
 * functions are registered in that Lua state too and dll/glue.lua is executed. */
#define ADDR_GLUE_REGISTER_CALL  0x0046A880u /* call 0x46ABB0 (glue script functions) */
#define ADDR_GLUE_REGISTER       0x0046ABB0u
#define ADDR_FRAMESCRIPT_EXECUTE 0x00704CD0u /* fastcall(code, chunk name), as RunScript */
#define ADDR_CREATE_COMPONENT    0x00B42078u /* CCharacterComponent* of the character being created */
#define ADDR_CREATE_SCENE        0x00B420F0u /* character create scene */
#define ADDR_CREATE_APPLY        0x00470B70u /* fastcall(desc, 1): new component + model from desc (sex change path) */
#define COMPONENT_DESC           0x18        /* copy of the description (0x5B dwords, 0x476B90) */
#define DESC_SIZE                0x16C
#define COMPONENT_MODEL          0x38
/* Character select keeps one component + model per listed character (entry +0x110)
 * and only builds one when the slot is empty (0x472A67), so re-selecting shows the
 * old model. Releasing them through the game's own release (also used when the
 * list is refreshed) makes the next selection build the character from the new files. */
#define ADDR_CHARLIST            0x00B42144u /* character entries, 0x120 bytes each */
#define ADDR_CHARLIST_COUNT      0x00B42140u
#define CHARLIST_STRIDE          0x120
#define CHARENTRY_COMPONENT      0x110
#define CHARENTRY_MODEL          0x114       /* released by 0x473700 but not cleared */
#define ADDR_CHARENTRY_RELEASE   0x00473700u /* thiscall(entry) */
typedef void (__fastcall *CharEntryRelease_t)(void *entry, void *unused);
typedef void (__fastcall *FrameScriptExecute_t)(const char *code, const char *name);
typedef void (__fastcall *CreateApply_t)(void *desc, int rebuild);
#include "glue_lua.h"

/* Rebuilds the character on the create screen from its current description, the
 * way the game does on a sex change, so race, sex, skin, face and hair stay as
 * they are while the model, textures and skin are read with the new selection. */
static int rebuild_create_character(void)
{
	uint8_t *comp = *(uint8_t **)ADDR_CREATE_COMPONENT;
	if (!comp || !*(void **)ADDR_CREATE_SCENE || !*(void **)(comp + COMPONENT_MODEL))
		return 0;
	uint8_t desc[DESC_SIZE];
	memcpy(desc, comp + COMPONENT_DESC, sizeof desc);
	((CreateApply_t)ADDR_CREATE_APPLY)(desc, 1);
	return 1;
}

static int release_select_characters(void)
{
	uint8_t *list = *(uint8_t **)ADDR_CHARLIST;
	uint32_t count = *(uint32_t *)ADDR_CHARLIST_COUNT;
	int n = 0;
	if (!list || count > 64)
		return 0;
	for (uint32_t i = 0; i < count; i++) {
		uint8_t *e = list + i * CHARLIST_STRIDE;
		if (!*(void **)(e + CHARENTRY_COMPONENT))
			continue;
		((CharEntryRelease_t)ADDR_CHARENTRY_RELEASE)(e, NULL);
		*(void **)(e + CHARENTRY_MODEL) = NULL; /* never released twice */
		n++;
	}
	return n;
}

/* HDPack_RefreshGlue("select" | "create" | "none") -> refreshed texture count | nil
 * Like HDPack_Refresh without the world steps. On the create screen it also
 * rebuilds the character; on the select screen it releases the stored
 * characters, and Lua re-selects the current one so it is built again. */
static int __fastcall lua_HDPack_RefreshGlue(void *L)
{
	const char *screen = lua_gettop(L) >= 1 ? lua_tostring(L, 1) : NULL;
	if (!g_hooksOk || !enforce_server()) {
		if (g_hooksOk && g_locked && (g_appliedMask || g_offMask))
			refresh_core(1);
		lua_pushnil(L);
		return 1;
	}
	LONG changed = g_wantMask ^ g_appliedMask;
	int n = refresh_core(1);
	if (changed && screen && !strcmp(screen, "create") && rebuild_create_character())
		hdlog("character create: character rebuilt");
	if (changed && screen && !strcmp(screen, "select")) {
		int r = release_select_characters();
		hdlog("character select: %d characters released, rebuilt on selection", r);
	}
	lua_pushnumber(L, n);
	return 1;
}

/* HDPack_Update() -> newer version string ("1.2.3") | nil (none, not checked yet, or off) */
static int __fastcall lua_HDPack_Update(void *L)
{
	if (g_updateDone && g_latest[0] && !g_locked)
		lua_pushstring(L, g_latest);
	else
		lua_pushnil(L);
	return 1;
}

/* HDPack_Info() -> version, hooks installed and allowed, status, locked */
static int __fastcall lua_HDPack_Info(void *L)
{
	int ok = enforce_server();
	lua_pushstring(L, HDT_VERSION);
	lua_pushboolean(L, g_hooksOk && ok);
	lua_pushstring(L, ok ? g_status : "not available on this server");
	lua_pushboolean(L, !ok);
	return 4;
}

/* --- paket denetimi -----------------------------------------------------------
 * HDPack_Scan(): Data klasöründeki harfli paketlerin (patch-?.mpq) SHA-256'sını arka planda hesaplar;
 * eklenti bunları desteklenen setle (PackRefs.lua, sürüm betiğinin ürettiği) karşılaştırır. Sonuç
 * boyut ve değişme zamanıyla ayar dosyasında ([PackHashes]) saklanır; değişmeyen paket yeniden okunmaz.
 * İş parçacığı kendi DLL referansını tutar ve son adımda bırakır (güncelleme kontrolüyle aynı). */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; uint32_t used; } Sha256;
static const uint32_t SHA_K[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(Sha256 *c, const uint8_t *p)
{
	uint32_t w[64], a, b, d, e, f, g, h, k, t1, t2;
	for (int i = 0; i < 16; i++)
		w[i] = (uint32_t)p[i * 4] << 24 | (uint32_t)p[i * 4 + 1] << 16 | (uint32_t)p[i * 4 + 2] << 8 | p[i * 4 + 3];
	for (int i = 16; i < 64; i++)
		w[i] = w[i - 16] + (ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3)) + w[i - 7] +
		       (ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10));
	a = c->h[0], b = c->h[1], k = c->h[2], d = c->h[3], e = c->h[4], f = c->h[5], g = c->h[6], h = c->h[7];
	for (int i = 0; i < 64; i++) {
		t1 = h + (ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25)) + ((e & f) ^ (~e & g)) + SHA_K[i] + w[i];
		t2 = (ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22)) + ((a & b) ^ (a & k) ^ (b & k));
		h = g, g = f, f = e, e = d + t1, d = k, k = b, b = a, a = t1 + t2;
	}
	c->h[0] += a, c->h[1] += b, c->h[2] += k, c->h[3] += d, c->h[4] += e, c->h[5] += f, c->h[6] += g, c->h[7] += h;
}

static void sha256_init(Sha256 *c)
{
	static const uint32_t iv[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
	                               0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
	memcpy(c->h, iv, sizeof iv);
	c->len = 0, c->used = 0;
}

static void sha256_update(Sha256 *c, const uint8_t *p, size_t n)
{
	c->len += n;
	if (c->used) {
		while (n && c->used < 64)
			c->buf[c->used++] = *p++, n--;
		if (c->used < 64)
			return;
		sha256_block(c, c->buf), c->used = 0;
	}
	for (; n >= 64; p += 64, n -= 64)
		sha256_block(c, p);
	memcpy(c->buf, p, n), c->used = (uint32_t)n;
}

static void sha256_hex(Sha256 *c, char out[65])
{
	uint64_t bits = c->len * 8;
	uint8_t pad = 0x80, zero = 0, lenb[8];
	sha256_update(c, &pad, 1);
	while (c->used != 56)
		sha256_update(c, &zero, 1);
	for (int i = 0; i < 8; i++)
		lenb[i] = (uint8_t)(bits >> (56 - i * 8));
	sha256_update(c, lenb, 8);
	for (int i = 0; i < 8; i++)
		snprintf(out + i * 8, 9, "%08x", (unsigned)c->h[i]);
}

#define SCAN_MAX 48
typedef struct { char name[24]; double size; char sha[65]; } ScanEntry; /* sha "" : okunamadı */
static ScanEntry g_scan[SCAN_MAX];
static volatile LONG g_scanState;           /* 0 hiç, 1 sürüyor, 2 bitti */
static volatile LONG g_scanCount, g_scanDoneMB, g_scanTotalMB;

static int scan_cmp(const void *a, const void *b) { return _stricmp(((const ScanEntry *)a)->name, ((const ScanEntry *)b)->name); }

/* Data klasörü: ayar dosyasında datadir varsa o (testler), yoksa oyunun klasöründeki Data */
static void data_dir(char *out, size_t size, const char *ini)
{
	GetPrivateProfileStringA("Test", "datadir", "", out, (DWORD)size, ini);
	if (out[0]) {
		size_t n = strlen(out);
		if (out[n - 1] != '\\' && n + 1 < size)
			out[n] = '\\', out[n + 1] = 0;
		return;
	}
	GetModuleFileNameA(NULL, out, (DWORD)size);
	char *slash = strrchr(out, '\\');
	snprintf(slash ? slash + 1 : out, size - (size_t)((slash ? slash + 1 : out) - out), "Data\\");
}

static DWORD WINAPI scan_thread(void *arg) /* arg: bu DLL, iş parçacığı boyunca referanslı */
{
	char ini[MAX_PATH + 32], dir[MAX_PATH + 32], path[MAX_PATH + 64], cached[128];
	ini_path(ini, sizeof ini, "HDToggle.ini");
	data_dir(dir, sizeof dir, ini);
	SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
	static ScanEntry found[SCAN_MAX];
	WIN32_FILE_ATTRIBUTE_DATA at[SCAN_MAX];
	int n = 0;
	snprintf(path, sizeof path, "%.290spatch-?.mpq", dir);
	WIN32_FIND_DATAA fd;
	HANDLE fh = FindFirstFileA(path, &fd);
	if (fh != INVALID_HANDLE_VALUE) {
		do {
			char c = fd.cFileName[6];
			if (n < SCAN_MAX && strlen(fd.cFileName) == 11 && ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) &&
			    !(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
				memset(&found[n], 0, sizeof found[n]);
				snprintf(found[n].name, sizeof found[n].name, "%s", fd.cFileName);
				n++;
			}
		} while (FindNextFileA(fh, &fd));
		FindClose(fh);
	}
	qsort(found, (size_t)n, sizeof found[0], scan_cmp);
	double total = 0;
	for (int i = 0; i < n; i++) {
		snprintf(path, sizeof path, "%.290s%.23s", dir, found[i].name);
		if (!GetFileAttributesExA(path, GetFileExInfoStandard, &at[i]))
			memset(&at[i], 0, sizeof at[i]);
		found[i].size = (double)at[i].nFileSizeHigh * 4294967296.0 + at[i].nFileSizeLow;
		total += found[i].size;
	}
	InterlockedExchange(&g_scanTotalMB, (LONG)(total / 1048576.0));
	InterlockedExchange(&g_scanDoneMB, 0);
	double done = 0;
	uint8_t *buf = malloc(1 << 20);
	for (int i = 0; i < n; i++) {
		ScanEntry *e = &found[i];
		char key[32], stamp[64];
		snprintf(key, sizeof key, "%s", e->name);
		for (char *k = key; *k; k++)
			if (*k >= 'A' && *k <= 'Z')
				*k += 32;
		snprintf(stamp, sizeof stamp, "%lu:%lu:%lu:%lu", at[i].nFileSizeHigh, at[i].nFileSizeLow,
		         at[i].ftLastWriteTime.dwHighDateTime, at[i].ftLastWriteTime.dwLowDateTime);
		GetPrivateProfileStringA("PackHashes", key, "", cached, sizeof cached, ini);
		size_t sl = strlen(stamp);
		if (!strncmp(cached, stamp, sl) && cached[sl] == ' ' && strlen(cached + sl + 1) == 64) {
			memcpy(e->sha, cached + sl + 1, 65);
		} else if (buf) {
			snprintf(path, sizeof path, "%.290s%.23s", dir, e->name);
			HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
			                       OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
			if (f != INVALID_HANDLE_VALUE) {
				Sha256 c;
				sha256_init(&c);
				DWORD got;
				double read = 0;
				while (ReadFile(f, buf, 1 << 20, &got, NULL) && got) {
					sha256_update(&c, buf, got);
					read += got;
					InterlockedExchange(&g_scanDoneMB, (LONG)((done + read) / 1048576.0));
				}
				CloseHandle(f);
				if (read == e->size) {
					sha256_hex(&c, e->sha);
					char line[160];
					snprintf(line, sizeof line, "%s %s", stamp, e->sha);
					WritePrivateProfileStringA("PackHashes", key, line, ini);
				}
			}
		}
		done += e->size;
		InterlockedExchange(&g_scanDoneMB, (LONG)(done / 1048576.0));
	}
	free(buf);
	memcpy(g_scan, found, sizeof found[0] * (size_t)n);
	InterlockedExchange(&g_scanCount, n);
	for (int i = 0; i < n; i++)
		hdlog("pack check: %s %.0f %s", g_scan[i].name, g_scan[i].size, g_scan[i].sha[0] ? g_scan[i].sha : "unreadable");
	InterlockedExchange(&g_scanState, 2);
	FreeLibraryAndExitThread((HMODULE)arg, 0);
	return 0;
}

/* HDPack_Scan([again]) -> "running"|"done"|"failed", MB done, MB total, number of packs found.
 * Starts the check on the first call (or with again=1 after one finished). */
static int __fastcall lua_HDPack_Scan(void *L)
{
	int again = lua_gettop(L) >= 1 && lua_tonumber(L, 1) == 1;
	if (g_scanState == 0 || (g_scanState == 2 && again)) {
		HMODULE self = NULL;
		InterlockedExchange(&g_scanState, 1);
		InterlockedExchange(&g_scanCount, 0);
		if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)scan_thread, &self)) {
			InterlockedExchange(&g_scanState, 0);
			lua_pushstring(L, "failed");
			return 1;
		}
		HANDLE t = CreateThread(NULL, 0, scan_thread, self, 0, NULL);
		if (!t) {
			FreeLibrary(self);
			InterlockedExchange(&g_scanState, 0);
			lua_pushstring(L, "failed");
			return 1;
		}
		CloseHandle(t);
	}
	lua_pushstring(L, g_scanState == 2 ? "done" : "running");
	lua_pushnumber(L, g_scanDoneMB);
	lua_pushnumber(L, g_scanTotalMB);
	lua_pushnumber(L, g_scanState == 2 ? g_scanCount : 0);
	return 4;
}

/* HDPack_ScanEntry(i) -> file name, size in bytes, SHA-256 (nil if it could not be read); i = 1..count */
static int __fastcall lua_HDPack_ScanEntry(void *L)
{
	int i = lua_gettop(L) >= 1 ? (int)lua_tonumber(L, 1) : 0;
	if (g_scanState != 2 || i < 1 || i > g_scanCount) {
		lua_pushnil(L);
		return 1;
	}
	const ScanEntry *e = &g_scan[i - 1];
	lua_pushstring(L, e->name);
	lua_pushnumber(L, e->size);
	if (e->sha[0])
		lua_pushstring(L, e->sha);
	else
		lua_pushnil(L);
	return 3;
}

static int __fastcall lua_HDPack_RefreshGlue(void *L);

/* Açılış teşhisi (bir kez, arayüz yüklenirken; yükleyici kilidi altında değil):
 * aynı adlı ikinci bir HDToggle kopyası (eski elle kurulumdan kalan, dlls.txt'te ikinci
 * satır) her zaman bildirilir; debug açıkken yüklü modüller ve dlls.txt yazılır. */
static void startup_diagnostics(void)
{
	static int done;
	if (done)
		return;
	done = 1;
	char me[MAX_PATH];
	GetModuleFileNameA(g_self, me, sizeof me);
	HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
	int copies = 0;
	if (snap != INVALID_HANDLE_VALUE) {
		MODULEENTRY32 e;
		e.dwSize = sizeof e;
		for (BOOL ok = Module32First(snap, &e); ok; ok = Module32Next(snap, &e)) {
			if (!_stricmp(e.szModule, "HDToggle.dll") && _stricmp(e.szExePath, me)) {
				copies++;
				hdlog("WARNING: another HDToggle.dll is loaded too: %s. Remove it and its line in dlls.txt; "
				      "only %s should be loaded", e.szExePath, me);
			}
			dbg1("module %s (%lu KB) %s", e.szModule, (unsigned long)(e.modBaseSize / 1024), e.szExePath);
		}
		CloseHandle(snap);
	}
	if (g_debug) {
		char path[MAX_PATH + 32];
		GetModuleFileNameA(NULL, path, sizeof path);
		char *sl = strrchr(path, '\\');
		snprintf(sl ? sl + 1 : path, sizeof path - (size_t)((sl ? sl + 1 : path) - path), "dlls.txt");
		FILE *f = fopen(path, "r");
		char line[300];
		hdlog("D1 debug level %d; this DLL: %s; game: %s", g_debug, me, path);
		while (f && fgets(line, sizeof line, f)) {
			line[strcspn(line, "\r\n")] = 0;
			hdlog("D1 dlls.txt: %s", line);
		}
		if (f)
			fclose(f);
	}
	(void)copies;
}

static void register_lua(void)
{
	startup_diagnostics();
	RegisterFunc_t reg = (RegisterFunc_t)ADDR_REGISTER_FUNC;
	reg("HDPack_List", (void *)lua_HDPack_List);
	reg("HDPack_Set", (void *)lua_HDPack_Set);
	reg("HDPack_Refresh", (void *)lua_HDPack_Refresh);
	reg("HDPack_Info", (void *)lua_HDPack_Info);
	reg("HDPack_Update", (void *)lua_HDPack_Update);
	reg("HDPack_Scan", (void *)lua_HDPack_Scan);
	reg("HDPack_ScanEntry", (void *)lua_HDPack_ScanEntry);
}

/* --- teşhis: oyunun bir dokuyu yükleyemeyip beyaz/varsayılanla değiştirdiği yerler.
 * Davranış değişmez; her doku adı bir kez günlüğe yazılır. */
#define ADDR_LOCKFAIL_CALL  0x004488FAu /* UpdateBlpTextureAsync: GxTex_Lock'ta dosya okunamadı */
#define ADDR_ASYNCFAIL_CALL 0x0044A513u /* yükleme tamamlanınca BLP işlenemedi */
void *g_origLockDefault; /* asm köprüsü kullanır */
static void (__fastcall *g_origAsyncDefault)(void *list, uint8_t *tex);
static char g_failSeen[64][96];
static int g_failCount;

static void note_texfail(const char *why, const char *name)
{
	if (!name)
		name = "?";
	for (int i = 0; i < g_failCount && i < 64; i++)
		if (!strncmp(g_failSeen[i], name, sizeof g_failSeen[i] - 1))
			return;
	if (g_failCount < 64)
		snprintf(g_failSeen[g_failCount], sizeof g_failSeen[0], "%s", name);
	g_failCount++;
	hdlog("texture failed to load (%s): %s", why, name);
}

void __cdecl hdt_note_lockfail(const char *name) { note_texfail("re-read", name); }

/* call 0x448920 yerine: esi = doku adı; kayıt alıp asıl fonksiyona atlar */
__asm__(".globl _hdt_lockfail_stub\n"
        "_hdt_lockfail_stub:\n"
        "  pushal\n"
        "  pushl %esi\n"
        "  call _hdt_note_lockfail\n"
        "  addl $4, %esp\n"
        "  popal\n"
        "  jmp *_g_origLockDefault\n");
void hdt_lockfail_stub(void);

static void __fastcall hook_async_default(void *list, uint8_t *tex)
{
	note_texfail("first load", tex ? (const char *)(tex + TEX_NAME) : NULL);
	g_origAsyncDefault(list, tex);
}

/* --- incomplete mip chains (white textures) ------------------------------------
 * Several HD pack BLPs that are not square (512x128 casting bar border, 512x256
 * daggers...) lack their last mip level: the 1x1 entry has size 0. The first load
 * tolerates it, but when a texture has to be uploaded again (device reset, window
 * or resolution change, a toggle) the game reloads the file synchronously
 * (0x4489A0), wants every level down to 1x1 (count from the size, 0x5A4AC0) and
 * fills the texture with white when one is missing ("Texture %s not loaded --
 * replaced with default"). With DXT every level of 4x4 or less is exactly one
 * block, so a missing one-block level is the previous one-block level: right after
 * that reload path reads the header (call 0x4489F6 -> 0x5A7B40) the in-memory mip
 * table is completed. Files are not changed. */
#define ADDR_BLP_OPEN_CALL 0x004489F6u /* in 0x4489A0: call CBLPFile::Open */
#define ADDR_BLP_OPEN      0x005A7B40u /* thiscall(blp, name, flag), ret 8 */
#define BLP_COMPRESSION    0x0C        /* byte: 2 = DXT */
#define BLP_HASMIPS        0x0F
#define BLP_WIDTH          0x10
#define BLP_HEIGHT         0x14
#define BLP_MIPOFS         0x18        /* 16 dwords */
#define BLP_MIPSIZE        0x58        /* 16 dwords */
typedef int (__thiscall *BlpOpen_t)(void *blp, const char *name, int flag);
static int g_mipFixed;

static int complete_mip_chain(uint8_t *blp)
{
	if (blp[BLP_COMPRESSION] != 2 || !blp[BLP_HASMIPS])
		return 0;
	uint32_t w = *(uint32_t *)(blp + BLP_WIDTH), h = *(uint32_t *)(blp + BLP_HEIGHT);
	uint32_t *ofs = (uint32_t *)(blp + BLP_MIPOFS), *size = (uint32_t *)(blp + BLP_MIPSIZE);
	if (!w || !h)
		return 0;
	int fixed = 0;
	uint32_t pw = w, ph = h;
	for (int i = 1; i < 16 && (pw > 1 || ph > 1); i++) {
		uint32_t lw = pw > 1 ? pw / 2 : 1, lh = ph > 1 ? ph / 2 : 1;
		/* both levels one DXT block: the data is interchangeable */
		if (!size[i] && size[i - 1] && pw <= 4 && ph <= 4) {
			ofs[i] = ofs[i - 1];
			size[i] = size[i - 1];
			fixed++;
		}
		pw = lw, ph = lh;
	}
	return fixed;
}

static int __fastcall hook_blp_open(uint8_t *blp, void *edx, const char *name, int flag)
{
	(void)edx;
	int r = ((BlpOpen_t)ADDR_BLP_OPEN)(blp, name, flag);
	if (r && complete_mip_chain(blp) && g_mipFixed++ < 64)
		hdlog("mip chain completed (missing last level in the pack): %s", name ? name : "?");
	return r;
}

/* --- stale texture info in the character texture cache (crash at 0x477FE8) ---------
 * Skin compositing does not read a section texture's size from the texture: it
 * goes through a TextureCache entry (TextureCache.cpp, CACHEENTRY) that copies the
 * info once (0x47B150: +0x1C width, +0x20 height, +0x24 format, +0x28 mip count...,
 * from TextureGetInfo 0x44B430) and keeps it while +0x1C is non-zero. A refresh
 * re-decodes such textures in place with the new file (other size, fewer mips);
 * the entry kept the old HD info, so the blit walked mip levels the new image
 * does not have and read address 2. At each of the 5 call sites the cached info is
 * compared with the texture and dropped when it differs; the game recomputes it. */
#define ADDR_TEXCACHE_GETINFO 0x0047B150u /* fastcall(entry, out, flag), ret 4 */
#define TEXCACHE_TEXTURE      0x18
#define TEXCACHE_W            0x1C
#define TEXCACHE_H            0x20
#define TEXCACHE_FORMAT       0x24
static const uintptr_t TEXCACHE_CALLS[] = {0x00476EF2u, 0x00476FA4u, 0x00477025u, 0x004770B2u, 0x0047712Du};
typedef int (__fastcall *TexCacheGetInfo_t)(uint8_t *entry, void *out, int flag);
static int g_texInfoDropped;

static int __fastcall hook_texcache_getinfo(uint8_t *entry, void *out, int flag)
{
	uint8_t *tex = entry ? *(uint8_t **)(entry + TEXCACHE_TEXTURE) : NULL;
	if (tex && *(uint32_t *)(entry + TEXCACHE_W) && *(void **)(tex + TEX_BITS) &&
	    (*(uint32_t *)(entry + TEXCACHE_W) != *(uint32_t *)(tex + TEX_W) ||
	     *(uint32_t *)(entry + TEXCACHE_H) != *(uint32_t *)(tex + TEX_H) ||
	     *(uint32_t *)(entry + TEXCACHE_FORMAT) != *(uint32_t *)(tex + TEX_FORMAT))) {
		if (g_texInfoDropped++ < 32 || g_debug)
			hdlog("texture cache: outdated size %ux%u -> %ux%u, recomputed: %s",
			      *(uint32_t *)(entry + TEXCACHE_W), *(uint32_t *)(entry + TEXCACHE_H),
			      *(uint32_t *)(tex + TEX_W), *(uint32_t *)(tex + TEX_H), (const char *)(tex + TEX_NAME));
		*(uint32_t *)(entry + TEXCACHE_W) = 0; /* the game recomputes it from the texture */
	}
	return ((TexCacheGetInfo_t)ADDR_TEXCACHE_GETINFO)(entry, out, flag);
}

typedef void (__fastcall *M2CacheClean_t)(uint8_t *cache, void *edx);
typedef void (__fastcall *AsyncCancel_t)(void *read, void *edx);
typedef void (__fastcall *M2SharedDtor_t)(uint8_t *model, void *edx);
static M2CacheClean_t g_origM2Clean;

/* LRU'daki eskimiş modelleri oyunun temizleyicisinin adımlarıyla siler. */
static int purge_stale_models(uint8_t *cache)
{
	int n = 0;
	uint8_t **slot = (uint8_t **)(cache + M2CACHE_LRU_HEAD);
	for (uint8_t *m; (m = *slot) != NULL;) {
		if (m[M2S_NAME] != M2_STALE_MARK) {
			slot = (uint8_t **)(m + M2S_LRU_NEXT);
			continue;
		}
		uint8_t *next = *(uint8_t **)(m + M2S_LRU_NEXT);
		*slot = next;
		if (next)
			*(uint8_t ***)(next + M2S_LRU_SLOT) = slot;
		else
			*(uint8_t ***)(cache + M2CACHE_LRU_TAILSLOT) = slot;
		*(void **)(m + M2S_LRU_SLOT) = NULL;
		*(void **)(m + M2S_LRU_NEXT) = NULL;
		void **read = *(void ***)(m + M2S_ASYNC);
		if (read) {
			void *file = *read;
			((AsyncCancel_t)ADDR_ASYNC_CANCEL)(read, NULL);
			*(void **)(m + M2S_ASYNC) = NULL;
			((SFile2Close_t)ADDR_SFILE2_CLOSE)(file);
		}
		((M2SharedDtor_t)ADDR_M2SHARED_DTOR)(m, NULL);
		((SMemFree_t)ADDR_SMEM_FREE)(m, "delete", -1, 0);
		n++;
	}
	return n;
}

static void __fastcall hook_m2cache_clean(uint8_t *cache, void *edx)
{
	if (cache && (LONG)(GetTickCount() - (DWORD)g_staleUntil) < 0) {
		int n = purge_stale_models(cache);
		if (n) {
			g_staleFreed += n;
			dbg1("%d stale models freed right away", n);
		}
	}
	g_origM2Clean(cache, edx);
}

static void hook_call_site(uintptr_t site, void *hook, void **orig)
{
	if (*(uint8_t *)site != 0xE8)
		return;
	*orig = (void *)(site + 5 + *(int32_t *)(site + 1));
	uint8_t c[5] = {0xE8};
	*(int32_t *)(c + 1) = (int32_t)((uintptr_t)hook - (site + 5));
	patch_bytes(site, c, sizeof c);
}

/* Doku havuzunun boyut sınırını oyunun kendi değerine döndürür (bkz. TEXPOOL_LIMIT).
 * Kilitliyken de yapılır: yalnızca orijinal oyun kodunu geri getirir. */
static void restore_texpool_limit(void)
{
	for (size_t i = 0; i < sizeof TEXPOOL_LIMIT / sizeof TEXPOOL_LIMIT[0]; i++) {
		const uint8_t *op = (const uint8_t *)(TEXPOOL_LIMIT[i] - 2);
		uint32_t cur = *(uint32_t *)TEXPOOL_LIMIT[i];
		if (op[0] != TEXPOOL_LIMIT_OP[i][0] || op[1] != TEXPOOL_LIMIT_OP[i][1]) {
			hdlog("texture pool limit at 0x%08X: unexpected code, left alone", (unsigned)TEXPOOL_LIMIT[i]);
			continue;
		}
		if (cur == TEXPOOL_LIMIT_VALUE)
			continue;
		uint32_t v = TEXPOOL_LIMIT_VALUE;
		patch_bytes(TEXPOOL_LIMIT[i], &v, 4);
		hdlog("texture pool size limit at 0x%08X restored to %u (was %u)", (unsigned)TEXPOOL_LIMIT[i], v, cur);
	}
}

static ComponentInit_t g_componentInit;
/* Birleştirme sistemi tamponunu (VanillaHelpers'ın en büyük ölçeğiyle) ayırdıktan
 * hemen sonra, ilk karakter çizilmeden önce yerleşimi seçime göre ayarla. */
static void hook_component_init(void)
{
	/* tampon g_componentInit içinde 0x44AF90 üzerinden ayrılır: sınır önce düzelmeli */
	restore_texpool_limit();
	/* tampon burada ayrılır: önce paketlerin ten ölçeğini ölç, VanillaHelpers'ın
	 * seçtiğinden büyük bir ölçek gerekiyorsa tamponu o ölçekle ayırt */
	g_vhMarker = scale_of(*(uint32_t *)VH_ALLOC_CONST);
	if (g_vhMarker >= 0 && !g_locked) {
		measure_pack_skins();
		int need = g_vhMarker;
		for (int b = 0; b < 26; b++)
			if (g_packSkin[b] > need)
				need = g_packSkin[b];
		if (need > SKIN_SCALE_MAX)
			need = SKIN_SCALE_MAX;
		if (need > g_vhMarker) {
			uint32_t v = 256u << need;
			for (size_t i = 0; i < sizeof VH_BUFFER / sizeof VH_BUFFER[0]; i++)
				patch_bytes(VH_BUFFER[i], &v, 4);
			hdlog("skin compositing buffer raised to %ux%u for %dx skins", v, v, 1 << need);
		}
	}
	g_componentInit();
	g_vhMax = scale_of(*(uint32_t *)VH_ALLOC_CONST);
	set_skin_layout(wanted_skin_scale(g_offMask));
	hdlog("skin compositing: buffer scale %d, layout %d", g_vhMax, scale_of(*(uint32_t *)VH_LAYOUT_256[1]));
}

static void hook_loadscript(void)
{
	g_loadScript();
	register_lua();
}

/* HDPack_Log("text"): glue.lua reports its errors here (those screens show none) */
static int __fastcall lua_HDPack_Log(void *L)
{
	const char *msg = lua_gettop(L) >= 1 ? lua_tostring(L, 1) : NULL;
	if (msg)
		hdlog("%.400s", msg);
	return 0;
}

static LoadScript_t g_glueRegister;
static void hook_glue_register(void)
{
	g_glueRegister();
	startup_diagnostics();
	RegisterFunc_t reg = (RegisterFunc_t)ADDR_REGISTER_FUNC;
	reg("HDPack_List", (void *)lua_HDPack_List);
	reg("HDPack_Set", (void *)lua_HDPack_Set);
	reg("HDPack_Info", (void *)lua_HDPack_Info);
	reg("HDPack_RefreshGlue", (void *)lua_HDPack_RefreshGlue);
	reg("HDPack_Log", (void *)lua_HDPack_Log);
	reg("HDPack_Update", (void *)lua_HDPack_Update);
	((FrameScriptExecute_t)ADDR_FRAMESCRIPT_EXECUTE)(GLUE_LUA, "HDSwitch");
}

/* --- kurulum --------------------------------------------------------------- */
static int patch_bytes(uintptr_t addr, const void *bytes, size_t len)
{
	DWORD old;
	if (!VirtualProtect((void *)addr, len, PAGE_EXECUTE_READWRITE, &old))
		return 0;
	memcpy((void *)addr, bytes, len);
	VirtualProtect((void *)addr, len, old, &old);
	FlushInstructionCache(GetCurrentProcess(), (void *)addr, len);
	return 1;
}

static int find_bytes(const void *needle, size_t len, uintptr_t from, uintptr_t to)
{
	for (uintptr_t p = from; p + len <= to; p++)
		if (memcmp((const void *)p, needle, len) == 0)
			return 1;
	return 0;
}

static int verify_build(void)
{
	static const char build[] = "WoW [Release] Build 5875";
	/* .rdata/.data aralığında sürüm metni */
	return find_bytes(build, sizeof build - 1, 0x007FF000u, 0x00870000u);
}

static int install_hooks(void)
{
	static const uint8_t searchPrologue[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x44, 0x02, 0x00, 0x00};
	static const uint8_t registerPrologue[4] = {0x56, 0x57, 0x8B, 0xF9};

	if (!verify_build()) {
		snprintf(g_status, sizeof g_status, "WoW.exe is not build 5875");
		return 0;
	}
	if (memcmp((void *)ADDR_STORM_SEARCH, searchPrologue, sizeof searchPrologue) != 0) {
		snprintf(g_status, sizeof g_status, "the Storm search function was changed by another mod");
		return 0;
	}
	if (memcmp((void *)ADDR_REGISTER_FUNC, registerPrologue, sizeof registerPrologue) != 0) {
		snprintf(g_status, sizeof g_status, "FrameScript_RegisterFunction is not where expected");
		return 0;
	}
	if (*(uint8_t *)ADDR_LOADSCRIPT_CALL != 0xE8) {
		snprintf(g_status, sizeof g_status, "unexpected LoadScriptFunctions call site");
		return 0;
	}

	/* Trampolin: orijinal ilk 9 bayt + 0x6549A9'a atlama */
	uint8_t *tramp = VirtualAlloc(NULL, 32, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	if (!tramp) {
		snprintf(g_status, sizeof g_status, "could not allocate memory");
		return 0;
	}
	memcpy(tramp, searchPrologue, sizeof searchPrologue);
	tramp[9] = 0xE9;
	*(int32_t *)(tramp + 10) = (int32_t)(ADDR_STORM_SEARCH + 9 - ((uintptr_t)tramp + 14));
	FlushInstructionCache(GetCurrentProcess(), tramp, 32);
	g_stormSearch = (StormSearch_t)tramp;

	/* LoadScriptFunctions: çağrı noktasındaki hedefi (başka bir mod
	 * değiştirmiş olabilir) zincirle. */
	int32_t rel = *(int32_t *)(ADDR_LOADSCRIPT_CALL + 1);
	g_loadScript = (LoadScript_t)(ADDR_LOADSCRIPT_CALL + 5 + rel);
	uint8_t call[5] = {0xE8};
	*(int32_t *)(call + 1) = (int32_t)((uintptr_t)hook_loadscript - (ADDR_LOADSCRIPT_CALL + 5));

	/* CCharacterComponent::Initialize çağrı noktası (yoksa ten ölçeği hiç değişmez) */
	if (*(uint8_t *)ADDR_COMPONENT_INIT_CALL == 0xE8) {
		int32_t crel = *(int32_t *)(ADDR_COMPONENT_INIT_CALL + 1);
		g_componentInit = (ComponentInit_t)(ADDR_COMPONENT_INIT_CALL + 5 + crel);
		uint8_t ccall[5] = {0xE8};
		*(int32_t *)(ccall + 1) = (int32_t)((uintptr_t)hook_component_init - (ADDR_COMPONENT_INIT_CALL + 5));
		patch_bytes(ADDR_COMPONENT_INIT_CALL, ccall, sizeof ccall);
	}

	/* karakter seçim / oluşturma ekranları: çağrı noktası beklenen hedefi göstermiyorsa (başka bir mod) eklenmez */
	if (*(uint8_t *)ADDR_GLUE_REGISTER_CALL == 0xE8 &&
	    ADDR_GLUE_REGISTER_CALL + 5 + *(int32_t *)(ADDR_GLUE_REGISTER_CALL + 1) == ADDR_GLUE_REGISTER) {
		g_glueRegister = (LoadScript_t)ADDR_GLUE_REGISTER;
		uint8_t gcall[5] = {0xE8};
		*(int32_t *)(gcall + 1) = (int32_t)((uintptr_t)hook_glue_register - (ADDR_GLUE_REGISTER_CALL + 5));
		patch_bytes(ADDR_GLUE_REGISTER_CALL, gcall, sizeof gcall);
	} else {
		hdlog("character screens: unexpected call site, button not added");
	}

	/* ten doku önbelleğindeki eskimiş boyut bilgisi: 5 çağrı noktası */
	for (size_t i = 0; i < sizeof TEXCACHE_CALLS / sizeof TEXCACHE_CALLS[0]; i++) {
		uintptr_t site = TEXCACHE_CALLS[i];
		if (*(uint8_t *)site == 0xE8 && site + 5 + *(int32_t *)(site + 1) == ADDR_TEXCACHE_GETINFO) {
			void *orig = NULL;
			hook_call_site(site, (void *)hook_texcache_getinfo, &orig);
		} else {
			hdlog("texture cache fix: call site 0x%08X changed by another mod, skipped", (unsigned)site);
		}
	}

	/* ten parçalarını bölgeye uydurma: 2 çağrı noktası */
	for (size_t i = 0; i < sizeof SKIN_PASTE_CALLS / sizeof SKIN_PASTE_CALLS[0]; i++) {
		uintptr_t site = SKIN_PASTE_CALLS[i];
		if (*(uint8_t *)site == 0xE8 && site + 5 + *(int32_t *)(site + 1) == ADDR_SKIN_PASTE) {
			void *orig = NULL;
			hook_call_site(site, (void *)hook_skin_paste, &orig);
			g_skinPaste = (SkinPaste_t)orig;
		} else {
			hdlog("skin part fitting: call site 0x%08X changed by another mod, skipped", (unsigned)site);
		}
	}

	/* eksik mip zinciri (beyaz dokular): yalnızca eşzamanlı yeniden yükleme yolu */
	{
		void *orig = NULL;
		hook_call_site(ADDR_BLP_OPEN_CALL, (void *)hook_blp_open, &orig);
		if (orig && orig != (void *)ADDR_BLP_OPEN) {
			/* başka bir mod bu çağrıyı değiştirmiş: geri al, dokunma */
			uint8_t c[5] = {0xE8};
			*(int32_t *)(c + 1) = (int32_t)((uintptr_t)orig - (ADDR_BLP_OPEN_CALL + 5));
			patch_bytes(ADDR_BLP_OPEN_CALL, c, sizeof c);
			hdlog("mip fix: call site changed by another mod, not installed");
		}
	}

	/* eskimiş modelleri 30 sn bekletmeden sil: önbellek temizleyicisinin çağrı noktası */
	if (*(uint8_t *)ADDR_M2CACHE_CLEAN_CALL == 0xE8 &&
	    ADDR_M2CACHE_CLEAN_CALL + 5 + *(int32_t *)(ADDR_M2CACHE_CLEAN_CALL + 1) == ADDR_M2CACHE_CLEAN) {
		void *orig = NULL;
		hook_call_site(ADDR_M2CACHE_CLEAN_CALL, (void *)hook_m2cache_clean, &orig);
		g_origM2Clean = (M2CacheClean_t)orig;
	} else {
		hdlog("model cache: call site changed by another mod, stale models are freed by the game after 30 s");
	}

	/* teşhis kancaları */
	hook_call_site(ADDR_LOCKFAIL_CALL, (void *)hdt_lockfail_stub, &g_origLockDefault);
	hook_call_site(ADDR_ASYNCFAIL_CALL, (void *)hook_async_default, (void **)&g_origAsyncDefault);

	uint8_t jmp[9] = {0xE9, 0, 0, 0, 0, 0xCC, 0xCC, 0xCC, 0xCC};
	*(int32_t *)(jmp + 1) = (int32_t)((uintptr_t)hook_search - (ADDR_STORM_SEARCH + 5));

	if (!patch_bytes(ADDR_LOADSCRIPT_CALL, call, sizeof call) ||
	    !patch_bytes(ADDR_STORM_SEARCH, jmp, sizeof jmp)) {
		snprintf(g_status, sizeof g_status, "could not patch code");
		return 0;
	}
	snprintf(g_status, sizeof g_status, "ready");
	return 1;
}

/* --- öz-sınama -------------------------------------------------------------
 * HDToggle.ini'de [Test] selftest=1 varsa, oyun arşivleri yükledikten sonra her
 * "tN=X|Yol|açıkBoyut|kapalıBoyut" satırı için dosyanın paket açıkken ve X
 * kapalıyken hangi boyutta okunduğunu ölçer, beklenenle karşılaştırıp
 * HDToggle.log'a yazar. Kullanıcının ayarına (g_offMask) dokunmaz: arşivi
 * kendisi seçer ve dosyayı o arşivden açar (belirli arşiv aramaları kancadan
 * geçmez). Giriş yapmadan gerçek Storm ile çekirdek mekanizmayı doğrular. */
static void *natural_owner(const char *name)
{
	ArcInfo list[64];
	int n = snapshot(list, 64);
	for (int i = 0; i < n; i++) {
		int r = archive_lookup(list[i].arc, name);
		if (r)
			return r == 1 ? list[i].arc : NULL;
	}
	return NULL;
}

static uint32_t size_in(void *arc, const char *name)
{
	void *h = NULL;
	if (!arc || !((SFileOpen_t)ADDR_SFILE_OPEN)(arc, name, 0, &h) || !h)
		return 0;
	uint32_t hi = 0, sz = ((SFileSize_t)ADDR_SFILE_SIZE)(h, &hi);
	((SFileClose_t)ADDR_SFILE_CLOSE)(h);
	return sz;
}

static const char *arc_name(void *arc)
{
	if (!arc)
		return "-";
	const char *p = (const char *)((uintptr_t)arc + ARC_NAME), *b = p;
	for (; *p; p++)
		if (*p == '\\' || *p == '/')
			b = p + 1;
	return b;
}

static DWORD WINAPI selftest_thread(void *unused)
{
	(void)unused;
	char path[MAX_PATH + 32], key[16], val[400];
	ini_path(path, sizeof path, "HDToggle.ini");
	ArcInfo list[64];
	int n = 0;
	for (int w = 0; w < 240; w++) { /* arşivler yüklenene kadar (en çok 2 dk) */
		n = snapshot(list, 64);
		int letters = 0;
		for (int i = 0; i < n; i++)
			letters += list[i].bit >= 0;
		if (letters > 0 && w > 10)
			break;
		Sleep(500);
	}
	Sleep(3000);
	n = snapshot(list, 64);
	hdlog("selftest: %d archives (by priority)", n);
	for (int i = 0; i < n; i++)
		hdlog("  %4d  %s", list[i].prio, (const char *)((uintptr_t)list[i].arc + ARC_NAME));
	int pass = 0, fail = 0;
	for (int t = 1; t < 200; t++) {
		snprintf(key, sizeof key, "t%d", t);
		GetPrivateProfileStringA("Test", key, "", val, sizeof val, path);
		if (!val[0])
			break;
		char *f1 = strchr(val, '|'), *f2 = f1 ? strchr(f1 + 1, '|') : NULL, *f3 = f2 ? strchr(f2 + 1, '|') : NULL;
		int b = letter_bit(val[0]);
		if (b < 0 || !f1 || !f2 || !f3)
			continue;
		*f2 = 0, *f3 = 0;
		const char *file = f1 + 1;
		uint32_t expOn = strtoul(f2 + 1, NULL, 10), expOff = strtoul(f3 + 1, NULL, 10);
		void *aOn = natural_owner(file);
		void *aOff = resolve(file, 1L << b);
		if (!aOff)
			aOff = aOn;
		uint32_t on = size_in(aOn, file), off = size_in(aOff, file);
		int ok = on == expOn && off == expOff;
		ok ? pass++ : fail++;
		hdlog("%s %c %s  on=%u(%s) off=%u(%s)  expected %u/%u", ok ? "PASS" : "FAIL", 'A' + b, file,
		      on, arc_name(aOn), off, arc_name(aOff), expOn, expOff);
	}
	hdlog("selftest done: %d passed, %d failed", pass, fail);
	return 0;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID reserved)
{
	(void)reserved;
	if (reason != DLL_PROCESS_ATTACH)
		return TRUE;
	g_self = inst;
	DisableThreadLibraryCalls(inst);
	InitializeCriticalSection(&g_logLock);
	GetModuleFileNameA(inst, g_dir, sizeof g_dir);
	char *slash = strrchr(g_dir, '\\');
	if (slash)
		slash[1] = 0;
	else
		g_dir[0] = 0;

	char logPath[MAX_PATH + 32];
	snprintf(logPath, sizeof logPath, "%sHDToggle.log", g_dir);
	DeleteFileA(logPath);

	read_server_files();
	if (!server_verdict(g_fileHost, g_fileRealm))
		g_locked = 1;
	load_settings();
	g_hooksOk = install_hooks();
	if (g_locked)
		hdlog("server not supported (%s): HD Switch locked", g_fileHost[0] ? g_fileHost : "no realm list");
	char letters[27];
	mask_to_letters(g_wantMask, letters);
	hdlog("HDToggle %s: %s (packs off: %s)", HDT_VERSION, g_status, letters[0] ? letters : "none");
	{
		const uint8_t *exe = (const uint8_t *)GetModuleHandleA(NULL);
		const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
		int laa = (nt->FileHeader.Characteristics & IMAGE_FILE_LARGE_ADDRESS_AWARE) != 0;
		hdlog("WoW.exe large address aware: %s", laa ? "yes" : "NO (the game can use only 2 GB)");
		memlog("at start");
		g_memNext = (LONG)GetTickCount() + (g_debug ? 60000 : 300000);
	}

	char ini[MAX_PATH + 32];
	ini_path(ini, sizeof ini, "HDToggle.ini");
	if (g_hooksOk && !g_locked && GetPrivateProfileIntA("HDToggle", "updatecheck", 1, ini))
	{
		/* the thread holds its own reference to the DLL and drops it as its last
		 * step (FreeLibraryAndExitThread), so the DLL can never be unloaded under it */
		HMODULE self = NULL;
		if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)(void *)update_thread, &self)) {
			HANDLE t = CreateThread(NULL, 0, update_thread, self, 0, NULL);
			if (t)
				CloseHandle(t);
			else
				FreeLibrary(self), InterlockedExchange(&g_updateDone, 1);
		} else {
			InterlockedExchange(&g_updateDone, 1);
		}
	}
	else
		InterlockedExchange(&g_updateDone, 1);
	if (g_hooksOk && GetPrivateProfileIntA("Test", "selftest", 0, ini))
		CloseHandle(CreateThread(NULL, 0, selftest_thread, NULL, 0, NULL));
	return TRUE;
}
