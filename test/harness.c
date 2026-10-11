/*
 * HDToggle.dll test düzeneği. WoW.exe 1.12.1'in DLL'in dokunduğu her yerini aynı
 * adreste ve aynı çağrı kuralıyla taklit eder, DLL'i yükler ve davranışını
 * sınar. Oyun gerekmez; Wine altında çalışır:  wine harness.exe HDToggle.dll
 *
 * Taklit edilenler:
 *   0x6549A0  Storm arama (fastcall, 5 yığın arg, ret 0x14; gerçek ilk 9 bayt)
 *   0xC53FE0  arşiv listesi başı, 0xC54008 kilidi
 *   0x48FE9C  call LoadScriptFunctions -> 0x490250
 *   0x704120  FrameScript_RegisterFunction (gerçek ilk 4 bayt)
 *   0x6F3xxx  Lua API (fastcall)
 *   0xC0ED38  CGxDevice* (vtable[11] thiscall, +0x31C taban mip)
 *   sürüm metni .rdata aralığında
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int g_fail, g_count;
#define CHECK(c, name) do { g_count++; if (c) printf("  tamam  %s\n", name); \
	else { g_fail++; printf("  HATA   %s  (%s:%d)\n", name, __FILE__, __LINE__); } } while (0)

/* --- sabit adreslerde bellek --------------------------------------------
 * Düzenek 0x400000'a yüklenir; WoW'un kullanılan aralığını (0x480000-0xCF0000)
 * kendi ".wow" bölümü olarak taşır (0x401000-0xD01000), böylece adresler başka bir şeye çarpmaz. */
__attribute__((section(".wow"), aligned(4096))) uint8_t g_wowArea[0x00900000] = {1};

static void *commit_at(uintptr_t addr, size_t size)
{
	uintptr_t lo = (uintptr_t)g_wowArea, hi = lo + sizeof g_wowArea;
	DWORD old;
	if (addr < lo || addr + size > hi) {
		printf("adres .wow bolumu disinda: %08x (bolum %08x-%08x)\n", (unsigned)addr, (unsigned)lo, (unsigned)hi);
		ExitProcess(2);
	}
	VirtualProtect((void *)addr, size, PAGE_EXECUTE_READWRITE, &old);
	return (void *)addr;
}

static void put(uintptr_t addr, const void *bytes, size_t n) { memcpy((void *)addr, bytes, n); }

static void put_jmp(uintptr_t at, const void *target)
{
	uint8_t j[5] = {0xE9};
	*(int32_t *)(j + 1) = (int32_t)((uintptr_t)target - (at + 5));
	put(at, j, 5);
}

/* --- sahte Storm arşivleri ------------------------------------------------ */
#define N_ARC 8
typedef struct {
	const char *path;
	int prio;
	const char *files[8]; /* içerdiği dosyalar */
} FakeArc;

/* Öncelik sırası (yüksekten düşüğe) - gerçek oyundaki gibi harfli paketler üstte.
 * "-" ile başlayan ad: o arşivde dosyanın "silindi" işareti (Storm 3 döndürür). */
static const FakeArc ARCS[N_ARC] = {
	{"Data\\patch-Z.mpq", 80, {"DBFilesClient\\SoundEntries.dbc"}},
	{"Data\\patch-T.mpq", 79, {"Creature\\Skin.blp", "Item\\Sword.blp", "-Item\\Old.blp", "DBFilesClient\\CreatureDisplayInfo.dbc", "DBFilesClient\\CreatureModelData.dbc", "VanillaHelpers\\ResizeCharacterSkin.txt", "Character\\Face.blp", "Character\\Troll\\Female\\TrollFemaleSkin00_03.blp"}},
	{"Data\\patch-C.mpq", 78, {"Creature\\Wolf.m2", "Creature\\NewWolf.m2", "Creature\\Skin.blp", "Creature\\Gone.m2", "DBFilesClient\\SoundEntries.dbc", "Character\\Human\\Female\\HumanFemaleSkin00_00.blp", "Creature\\Bat.m2"}},
	{"Data\\patch-5.mpq", 70, {"Creature\\Wolf.m2", "-Creature\\Gone.m2"}},
	{"Data\\patch.MPQ", 60, {"Creature\\Skin.blp", "Item\\Sword.blp", "DBFilesClient\\SoundEntries.dbc", "DBFilesClient\\CreatureDisplayInfo.dbc", "DBFilesClient\\CreatureModelData.dbc", "Character\\Face.blp"}},
	{"Data\\model.MPQ", 10, {"Creature\\Wolf.m2", "Creature\\Skin.blp", "Item\\Old.blp", "Creature\\Gone.m2", "Creature\\Bat.m2"}},
	/* düzeltme paketi: dizinin sonunda (mevcut indeksler değişmesin), öncelikte T ile C arasında */
	{"Data\\patch-H.mpq", 78, {"Creature\\Bat.m2", "Creature\\Glow.blp"}},
	/* U'nun düzeltme paketi: öncelikte Z ile T arasında (U'nun yerine T duruyor) */
	{"Data\\Patch-V.mpq", 79, {"Character\\Face.blp", "Character\\VOnly.blp"}},
};
/* arşiv listesinin öncelik sırası (dizi indeksleri) */
static const int ORDER[N_ARC] = {0, 7, 1, 6, 2, 3, 4, 5};
static uint8_t *g_arc[N_ARC];

static int arc_index(void *arc)
{
	for (int i = 0; i < N_ARC; i++)
		if ((void *)g_arc[i] == arc)
			return i;
	return -1;
}

/* 0 yok, 1 var, 3 silindi işareti */
static int arc_has(int i, const char *rawName)
{
	char name[300]; /* Storm gibi: / ile \ aynı */
	size_t n = 0;
	for (; rawName[n] && n < sizeof name - 1; n++)
		name[n] = rawName[n] == '/' ? '\\' : rawName[n];
	name[n] = 0;
	for (int k = 0; k < 8 && ARCS[i].files[k]; k++) {
		const char *f = ARCS[i].files[k];
		if (f[0] == '-' && _stricmp(f + 1, name) == 0)
			return 3;
		if (_stricmp(f, name) == 0)
			return 1;
	}
	return 0;
}

static int g_searchCalls, g_lastServed = -2;
static uint32_t g_lastArgs[5];

/* Orijinal arama: arc==0 ise öncelik sırasıyla ilk sahibi, değilse yalnızca o arşivi
 * arar. a3 bir çıkış göstergesiyse (gerçekte dosya tutamacı) sahibin indeksini yazar. */
static int __fastcall real_search(void *arc, const char *name, uint32_t a1, uint32_t a2,
                                  uint32_t a3, uint32_t a4, uint32_t a5)
{
	g_searchCalls++;
	uint32_t args[5] = {a1, a2, a3, a4, a5};
	/* gerçek Storm gibi: çıkışlar başta sıfırlanır (a3 burada sahip indeksi) */
	if (a2 >= 0x10000) /* served() argüman testinde sahte değerler (0x22, 0x44) geçer */
		*(uint32_t *)a2 = 0;
	if (a4 >= 0x10000)
		*(uint32_t *)a4 = 0;
	int from = 0, to = N_ARC, one = -1;
	if (arc) {
		one = arc_index(arc);
		if (one < 0)
			return 0; /* geçersiz tutamaç */
		from = 0, to = 1;
	}
	for (int k = from; k < to; k++) {
		int i = one >= 0 ? one : ORDER[k];
		int h = arc_has(i, name);
		if (h == 3)
			return 3; /* Storm: silinmiş, arama burada biter */
		if (h) {
			if (a3) {
				*(int *)a3 = i;
				g_lastServed = i;
				memcpy(g_lastArgs, args, sizeof args);
			}
			return 1;
		}
	}
	return 0;
}

/* --- sahte SFile (öz-sınama için) ------------------------------------------
 * Dosya boyutu = arşiv indeksi * 1000 + ad uzunluğu: hangi arşivden okunduğu
 * boyuttan anlaşılır. */
static int __stdcall sf_open(void *arc, const char *name, uint32_t scope, void **h)
{
	(void)scope;
	int out = -1;
	if (real_search(arc, name, 0, 0, (uint32_t)(uintptr_t)&out, 0, 0) != 1)
		return 0;
	*h = (void *)(uintptr_t)(((out + 1) << 16) | (uint32_t)strlen(name));
	return 1;
}
static uint32_t __stdcall sf_size(void *h, uint32_t *hi)
{
	if (hi)
		*hi = 0;
	return (uint32_t)((((uintptr_t)h >> 16) - 1) * 1000 + ((uintptr_t)h & 0xFFFF));
}
static int __stdcall sf_close(void *h) { (void)h; return 1; }
/* SFileReadFileEx: temel ten dokularının BLP başlığı; T'de 2x (512), C'de 4x (1024) */
static int g_sfReads;
static int __stdcall sf_read(void *h, void *buf, uint32_t size, uint32_t *got, void *ov, int a, int b)
{
	(void)ov; (void)a; (void)b;
	g_sfReads++;
	int arc = (int)((uintptr_t)h >> 16) - 1;
	uint8_t head[20] = {'B', 'L', 'P', '2', 1, 0, 0, 0, 1, 8, 8, 1};
	uint32_t side = arc == 1 ? 512 : arc == 2 ? 1024 : 256;
	memcpy(head + 12, &side, 4);
	memcpy(head + 16, &side, 4);
	memcpy(buf, head, size < sizeof head ? size : sizeof head);
	*got = size < sizeof head ? size : sizeof head;
	return 1;
}

/* --- sahte doku sistemi -----------------------------------------------------
 * CTexture: +0x0C ad, +0x110 bayrak, +0x138 yükleme nesnesi, +0x140 GxTex,
 * +0x150 GxTex bayrakları, +0x154 liste bağlantısı.
 * CGxTex (0x54): +1 yeniden oluştur, +4 bölge, +0x24 genişlik, +0x28 yükseklik,
 * +0x34 biçim, +0x48 D3D. "Dosya içeriği" ARC=<arşiv>; arşiv indeksi boyutu
 * belirler (genişlik = 64 << arşiv), böylece hangi dosyanın kurulduğu görülür. */
#define TEX_LINK 0x154
static int g_released, g_processed, g_d3dReleased, g_dirtyCalls, g_dirtyImmediate = -1, g_flagsAtProcess = -1;
static uint8_t *g_lastReleased, *g_lastDirty;
static int32_t g_lastDirtyRect[4];

static char g_sf2Name[300];
static int __stdcall sf2_open(int zero, const char *name, int flag, void **h)
{
	(void)zero; (void)flag;
	int out = -1;
	strncpy(g_sf2Name, name, sizeof g_sf2Name - 1);
	if (((int (__fastcall *)(void *, const char *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))0x006549A0)(
	        0, name, 0, 0, (uint32_t)(uintptr_t)&out, 0, 0) != 1)
		return 0;
	*h = (void *)(uintptr_t)(out + 1);
	return 1;
}
static uint32_t __stdcall sf2_size(void *h, uint32_t *hi) { (void)h; if (hi) *hi = 0; return 8; }
static int __stdcall sf2_read(void *h, void *buf, uint32_t size, uint32_t *got, void *ov, int u)
{
	(void)ov; (void)u;
	if (strstr(g_sf2Name, "ResizeCharacterSkin"))
		((char *)buf)[0] = '2'; /* VanillaHelpers işaret dosyası: ölçek 2x */
	else
		snprintf(buf, size, "ARC=%d", (int)(uintptr_t)h - 1);
	*got = size;
	return 1;
}
static void *g_lastClosed;
static int __stdcall sf2_close(void *h) { g_lastClosed = h; return 1; }

/* model önbelleği temizleyicisi (0x706870), okuma iptali (0x4439B0), model yıkıcısı (0x71D330) */
static int g_m2Cleans, g_m2Dtors, g_asyncCancels;
static uint8_t *g_m2DtorSeen[8];
static void *g_lastCancelled;
static void __fastcall m2cache_clean(uint8_t *cache, void *edx) { (void)cache; (void)edx; g_m2Cleans++; }
static void __fastcall async_cancel(void *read, void *edx) { (void)edx; g_lastCancelled = read; g_asyncCancels++; }
static void __fastcall m2shared_dtor(uint8_t *m, void *edx)
{
	(void)edx;
	if (g_m2Dtors < 8)
		g_m2DtorSeen[g_m2Dtors] = m;
	g_m2Dtors++;
}

/* sahte D3D doku: vtable[2] = Release */
static void __stdcall d3d_release(void *self) { (void)self; g_d3dReleased++; }
static void *g_d3dVt[3] = {0, 0, (void *)d3d_release};
static void *g_d3dObj = g_d3dVt;   /* nesnenin ilk alanı vtable */
static void *g_d3dStub = (void *)0x5757;

static uint8_t *new_gx(int arc)
{
	uint8_t *gx = calloc(1, 0x54);
	*(int32_t *)(gx + 0x24) = 64 << arc;     /* genişlik */
	*(int32_t *)(gx + 0x28) = 32 << arc;     /* yükseklik */
	*(int32_t *)(gx + 0x34) = arc;           /* biçim */
	*(int32_t *)(gx + 0x3c) = 0x100 + arc;   /* bayrak */
	*(void **)(gx + 0x48) = &g_d3dObj;
	return gx;
}
static int gx_arc(uint8_t *gx) { return gx ? *(int32_t *)(gx + 0x34) : -9; }

static int __fastcall tex_process(uint8_t *tex, void *edx_unused)
{
	(void)edx_unused;
	g_processed++;
	g_flagsAtProcess = *(uint32_t *)(tex + 0x110);
	void **fake = *(void ***)(tex + 0x138);
	const char *buf = fake ? fake[1] : NULL;
	if (!buf || strncmp(buf, "ARC=", 4) != 0)
		return 0;
	*(uint8_t **)(tex + 0x140) = new_gx(atoi(buf + 4));
	return 1;
}
static void __fastcall gx_release(uint8_t *gx) { g_released++; g_lastReleased = gx; }

/* aygıt: thiscall(dev, gx, w, h) boyut; thiscall(dev, gx, rect, hemen) kirli */
static uint32_t __fastcall dev_texsize(void *dev, void *edx, uint8_t *gx, int w, int h)
{
	(void)dev; (void)edx; (void)w; (void)h;
	return (uint32_t)(*(int32_t *)(gx + 0x24) * *(int32_t *)(gx + 0x28));
}
static void __fastcall dev_texdirty(void *dev, void *edx, uint8_t *gx, const int32_t *rect, int immediate)
{
	(void)dev; (void)edx;
	g_dirtyCalls++;
	g_lastDirty = gx;
	memcpy(g_lastDirtyRect, rect, sizeof g_lastDirtyRect);
	g_dirtyImmediate = immediate;
}

static uint8_t *g_m2[4];
/* sahte model önbelleği: Wolf (C'de, patch-5'te), NewWolf (yalnız C), Skin? yok.
 * İkisi aynı kovada zincir; biri tek. */
static void build_models(void)
{
	memset((void *)0x00CEEFC4, 0, 1021 * 4);
	const char *names[4] = {"Creature\\Wolf", "Creature\\NewWolf", "Item\\Sword", "Creature\\Gone"};
	for (int i = 0; i < 4; i++) {
		g_m2[i] = calloc(1, 0x164);
		strcpy((char *)g_m2[i] + 0x20, names[i]);
		*(int *)g_m2[i] = 1;
		*(uint32_t *)(g_m2[i] + 8) = 1; /* yüklü */
	}
	/* kova 7: Wolf -> NewWolf -> Sword ; kova 9: Gone */
	uint8_t **slot7 = (uint8_t **)(0x00CEEFC4 + 7 * 4), **slot9 = (uint8_t **)(0x00CEEFC4 + 9 * 4);
	*slot7 = g_m2[0];
	*(uint8_t ***)(g_m2[0] + 0x124) = slot7;
	*(uint8_t **)(g_m2[0] + 0x128) = g_m2[1];
	*(uint8_t ***)(g_m2[1] + 0x124) = (uint8_t **)(g_m2[0] + 0x128);
	*(uint8_t **)(g_m2[1] + 0x128) = g_m2[2];
	*(uint8_t ***)(g_m2[2] + 0x124) = (uint8_t **)(g_m2[1] + 0x128);
	*slot9 = g_m2[3];
	*(uint8_t ***)(g_m2[3] + 0x124) = slot9;
}

/* sahte nesne yöneticisi: 2 karakter, 1 eşya, 1 kaydı olmayan karakter, 1 kurulamayan */
static uint8_t g_objmgr[0x100], *g_obj[5], g_rec[2][0x40];
static int g_rebuilt, g_rebuildSawFakeOk;
static int g_step, g_lastRebuildStep; /* adım sırası: kurma dünya boşaltmadan önce mi */
static int g_unlinkNpcOnRebuild, g_nestedRefreshOnRebuild;
static void nested_refresh(void);
static void __fastcall unit_rebuild(uint8_t *u, void *edx)
{
	(void)edx;
	g_lastRebuildStep = ++g_step;
	if (g_unlinkNpcOnRebuild && u == g_obj[0]) {
		/* oyunun yan etkisi: başka bir nesne listeden çıkar ve belleği yeniden kullanılır */
		*(uintptr_t *)(g_obj[0] + 0x3C + 4) = (uintptr_t)g_obj[2];
		memset(g_obj[1], 0, 0x1000);
		g_unlinkNpcOnRebuild = 0;
	}
	if (g_nestedRefreshOnRebuild) {
		g_nestedRefreshOnRebuild = 0;
		nested_refresh();
	}
	uint8_t *rec = *(uint8_t **)(u + 0xB34);
	int idx = u == g_obj[0] ? 0 : u == g_obj[1] ? 1 : -1;
	if (u == g_obj[4])
		return; /* oyun kontrolde vazgeçti (ör. geçersiz görünüm) */
	if (idx >= 0 && rec != g_rec[idx]) {
		/* kopya: 0x0C hariç gerçek kayıtla aynı olmalı */
		if (*(uint32_t *)(rec + 0x0C) != *(uint32_t *)(g_rec[idx] + 0x0C) &&
		    !memcmp(rec, g_rec[idx], 0x0C) && !memcmp(rec + 0x10, g_rec[idx] + 0x10, 0x20))
			g_rebuildSawFakeOk++;
		*(uint8_t **)(u + 0xB34) = g_rec[idx];
		g_rebuilt++;
	}
}
static void build_objects(void)
{
	*(uint8_t **)0x00B41414 = g_objmgr;
	*(uint32_t *)(g_objmgr + 0xA4) = 0x3C;
	static uint8_t descs[5][16];
	for (int i = 0; i < 5; i++) {
		g_obj[i] = calloc(1, 0x1000);
		*(uint8_t **)(g_obj[i] + 8) = descs[i];
	}
	*(uint32_t *)(descs[0] + 8) = 0x19; /* oyuncu (nesne|karakter|oyuncu) */
	*(uint32_t *)(descs[1] + 8) = 0x09; /* NPC */
	*(uint32_t *)(descs[2] + 8) = 0x03; /* eşya */
	*(uint32_t *)(descs[3] + 8) = 0x09; /* kaydı yok */
	*(uint32_t *)(descs[4] + 8) = 0x09; /* kurulamayan */
	for (int r = 0; r < 2; r++)
		for (int k = 0; k < 0x40; k++)
			g_rec[r][k] = (uint8_t)(r * 50 + k);
	*(uint8_t **)(g_obj[0] + 0xB34) = g_rec[0];
	*(uint8_t **)(g_obj[1] + 0xB34) = g_rec[1];
	*(uint8_t **)(g_obj[4] + 0xB34) = g_rec[1];
	*(uint8_t **)(g_objmgr + 0xAC) = g_obj[0];
	for (int i = 0; i < 5; i++)
		*(uintptr_t *)(g_obj[i] + 0x3C + 4) = i + 1 < 5 ? (uintptr_t)g_obj[i + 1] : 1;
}


/* CBLPFile::Open taklidi: dosya adına göre gerçek paket başlıklarını doldurur */
static int g_blpOpens;
static int __fastcall blp_open(uint8_t *blp, void *e, const char *name, int flag)
{
	(void)e; (void)flag;
	g_blpOpens++;
	memset(blp, 0, 0x4C0);
	uint32_t *ofs = (uint32_t *)(blp + 0x18), *size = (uint32_t *)(blp + 0x58);
	if (!strcmp(name, "Border")) {         /* patch-I UI-CastingBar-Border: DXT5 512x128, 1x1 eksik */
		static const uint32_t o[] = {1172, 66708, 83092, 87188, 88212, 88468, 88532, 88564, 88580, 88596};
		static const uint32_t z[] = {65536, 16384, 4096, 1024, 256, 64, 32, 16, 16, 0};
		blp[0xC] = 2; blp[0xD] = 8; blp[0xE] = 7; blp[0xF] = 1;
		*(uint32_t *)(blp + 0x10) = 512; *(uint32_t *)(blp + 0x14) = 128;
		for (int i = 0; i < 10; i++) ofs[i] = o[i], size[i] = z[i];
		for (int i = 10; i < 16; i++) ofs[i] = 88596;
	} else if (!strcmp(name, "Dagger")) {  /* patch-G Knife_1H_Dagger: DXT1 512x256, 1x1 kaydı hiç yok */
		static const uint32_t o[] = {1172, 66708, 83092, 87188, 88212, 88468, 88532, 88548, 88556};
		static const uint32_t z[] = {65536, 16384, 4096, 1024, 256, 64, 16, 8, 8};
		blp[0xC] = 2; blp[0xF] = 1;
		*(uint32_t *)(blp + 0x10) = 512; *(uint32_t *)(blp + 0x14) = 256;
		for (int i = 0; i < 9; i++) ofs[i] = o[i], size[i] = z[i];
	} else if (!strcmp(name, "BigHole")) { /* 16x4 seviyesi eksik: tek blok değil, onarılmaz */
		blp[0xC] = 2; blp[0xF] = 1;
		*(uint32_t *)(blp + 0x10) = 64; *(uint32_t *)(blp + 0x14) = 16;
		ofs[0] = 100; size[0] = 512; ofs[1] = 612; size[1] = 128; ofs[2] = 740; size[2] = 0;
	} else if (!strcmp(name, "Palette")) { /* DXT değil: dokunulmaz */
		blp[0xC] = 1; blp[0xF] = 1;
		*(uint32_t *)(blp + 0x10) = 2; *(uint32_t *)(blp + 0x14) = 1;
		ofs[0] = 100; size[0] = 2;
	} else {
		return 0;                      /* açılamadı */
	}
	return 1;
}


/* TextureCache girdisi bilgi okuma taklidi: çağrıldığı anda girdideki genişliği kaydeder */
static int g_texInfoCalls;
static uint32_t g_texInfoSeenW;
static int __fastcall texcache_getinfo(uint8_t *entry, void *out, int flag)
{
	(void)out; (void)flag;
	g_texInfoCalls++;
	g_texInfoSeenW = *(uint32_t *)(entry + 0x1C);
	return 1;
}

/* --- ten parçası yapıştırma (0x477460 -> 0x477C80/0x477DB0) taklidi ---
 * Oyunun kopyalaması gibi: bölge boyutu kadar piksel, kaynağın mip sayısı kadar
 * seviye; her seviyede boyut (en az 1), konum ve satır aralıkları yarıya iner,
 * alfa maskesi (piksel başına 2 bit) seviyenin piksellerinin hemen ardında.
 * VanillaHelpers'ın sınır denetimi de taklit edilir: kaynak, istenen dikdörtgenden
 * küçükse hiç çizilmez. Hedef tampon 256<<B ölçeğinde ayrılır; dışına yazmalar sayılır. */
#define PB_LEVELS 11
static uint16_t *g_pbLevel[PB_LEVELS];
static uint32_t g_pbCount[PB_LEVELS], g_pbLevels;
static int g_pbOob, g_pbCalls, g_pbDrawn;
static void pb_alloc(int scale)
{
	g_pbLevels = 9 + (uint32_t)scale;
	for (uint32_t i = 0; i < PB_LEVELS; i++) {
		free(g_pbLevel[i]);
		g_pbLevel[i] = NULL, g_pbCount[i] = 0;
		if (i >= g_pbLevels)
			continue;
		uint32_t side = (256u << scale) >> i;
		g_pbCount[i] = side * side;
		g_pbLevel[i] = malloc(g_pbCount[i] * 2);
		for (uint32_t k = 0; k < g_pbCount[i]; k++)
			g_pbLevel[i][k] = 0xEEEE;
	}
	g_pbOob = g_pbCalls = g_pbDrawn = 0;
}
static void __fastcall mock_paste(void *comp, void *e, uint8_t **mips, int alpha, int32_t *dst, int32_t *src,
                                  int32_t *size, uint32_t *info)
{
	(void)comp; (void)e;
	g_pbCalls++;
	if ((uint32_t)src[0] >= info[0] || (uint32_t)src[1] >= info[1] || (uint32_t)size[0] > info[0] - (uint32_t)src[0] ||
	    (uint32_t)size[1] > info[1] - (uint32_t)src[1])
		return; /* VanillaHelpers: kaynak dışına okuma olacaksa çizme */
	g_pbDrawn++;
	int32_t dx = dst[0], dy = dst[1], sx = src[0], sy = src[1];
	uint32_t w = (uint32_t)size[0], h = (uint32_t)size[1], sp = info[0], dp = *(uint32_t *)0x00477CA1 / 2;
	for (uint32_t lv = 0; lv < info[3]; lv++) {
		if (lv >= g_pbLevels) {
			g_pbOob++; /* tamponun son seviyesinden sonrası: oyunda 0x477D15 çökmesi */
			break;
		}
		const uint16_t *s = (const uint16_t *)mips[lv];
		const uint8_t *mask = mips[lv] + h * sp * 2;
		uint32_t idx = 0;
		for (uint32_t y = 0; y < h; y++)
			for (uint32_t x = 0; x < w; x++, idx++) {
				if (alpha && !((mask[idx >> 2] >> ((idx & 3) * 2)) & 3))
					continue;
				uint32_t di = (uint32_t)(dy + (int32_t)y) * dp + (uint32_t)dx + x;
				if (di >= g_pbCount[lv]) {
					g_pbOob++;
					continue;
				}
				g_pbLevel[lv][di] = s[((uint32_t)sy + y) * sp + (uint32_t)sx + x];
			}
		w = w >> 1 ? w >> 1 : 1, h = h >> 1 ? h >> 1 : 1;
		dx >>= 1, dy >>= 1, sx >>= 1, sy >>= 1, sp >>= 1, dp >>= 1;
	}
}
/* sahte kaynak doku: seviye lv'de piksel = lv<<12 | (y&63)<<6 | (x&63); maske: (x+y) tek ise görünür */
static uint8_t **make_part(uint32_t w, uint32_t h, uint32_t levels, int withMask)
{
	uint8_t **m = calloc(levels, sizeof *m);
	for (uint32_t lv = 0; lv < levels; lv++) {
		uint32_t lw = w >> lv ? w >> lv : 1, lh = h >> lv ? h >> lv : 1, n = lw * lh;
		m[lv] = calloc(1, n * 2 + n / 4 + 4);
		uint16_t *p = (uint16_t *)m[lv];
		for (uint32_t y = 0; y < lh; y++)
			for (uint32_t x = 0; x < lw; x++) {
				p[y * lw + x] = (uint16_t)(lv << 12 | (y & 63) << 6 | (x & 63));
				if (withMask && ((x + y) & 1))
					m[lv][n * 2 + ((y * lw + x) >> 2)] |= (uint8_t)(3 << (((y * lw + x) & 3) * 2));
			}
	}
	return m;
}
static uint16_t part_px(uint32_t lv, uint32_t x, uint32_t y) { return (uint16_t)(lv << 12 | (y & 63) << 6 | (x & 63)); }

/* sahte sahne/dekor/oyun nesnesi */
static uint8_t g_scene[0x40]; /* +4: model önbelleği (kurulu sahne) */
static int g_modelReleased, g_cbCleared, g_lightCleared, g_doodadCreated, g_sceneCreated, g_setModel;
static char g_lastDoodadName[300], g_lastSceneName[300];
static uint8_t *g_lastDoodadDef, *g_lastSetModelObj, *g_lastSetModel;
static uint8_t *mk_model(uint8_t *shared, uintptr_t cb, void *arg)
{
	uint8_t *m = calloc(1, 0x428);
	*(uint8_t **)(m + 0x30) = shared;
	*(uintptr_t *)(m + 0x1F4) = cb;
	*(void **)(m + 0x1F8) = arg;
	*(uint8_t **)(m + 0x0C) = *(uint8_t **)(g_scene + 8); /* başa ekle */
	*(uint8_t **)(g_scene + 8) = m;
	return m;
}
static void __fastcall m_release(uint8_t *m, void *e) { (void)m; (void)e; g_modelReleased++; }

static void __fastcall m_setcb(uint8_t *m, void *e, void *fn, void *arg, int x)
{ (void)e; (void)x; if (!fn && !arg) g_cbCleared++; *(void **)(m + 0x1F4) = fn; *(void **)(m + 0x1F8) = arg; }
static void __fastcall m_setlight(uint8_t *m, void *e, void *fn, void *arg) { (void)m; (void)e; if (!fn && !arg) g_lightCleared++; }
static uint8_t g_newShared[0x164];
static int __fastcall doodad_create(const char *name, uint8_t *def, int flag)
{
	(void)flag;
	g_doodadCreated++;
	strcpy(g_lastDoodadName, name);
	g_lastDoodadDef = def;
	*(uint8_t **)(def + 0x88) = mk_model(g_newShared, 0x006951E0, def);
	return 1;
}
static void *__fastcall scene_create(uint8_t *scene, void *e, const char *name, int flags)
{
	(void)scene; (void)e; (void)flags;
	g_sceneCreated++;
	strcpy(g_lastSceneName, name);
	return mk_model(g_newShared, 0, NULL);
}
static void __fastcall obj_setmodel(uint8_t *obj, void *e, uint8_t *model)
{ (void)e; g_setModel++; g_lastSetModelObj = obj; g_lastSetModel = model; *(uint8_t **)(obj + 0xD8) = model; }
/* oyundaki gibi: thiscall, yığından argüman almaz (0x613E3D: mov ecx, obj; call [vt+0x28]) */
static const char *__fastcall go_modelname(uint8_t *obj, void *e) { (void)obj; (void)e; return "World\\Chest.mdx"; }
static void *g_goVt[16];
static int model_is_stale_mock(uint8_t *m)
{
	uint8_t *sh = *(uint8_t **)(m + 0x30);
	return sh && sh[0x20] == 1;
}

/* sahte bina önbelleği ve dünya */
static int g_unloadAll, g_unloadStep;
static void map_unload_all(void) { g_unloadAll++; g_unloadStep = ++g_step; }
/* karakter seçim / oluşturma (glue) */
static int g_glueRegisterCalls, g_execCount, g_createApplies, g_createFlag, g_createDescOk;
static char g_execName[32];
static const char *g_execCode;
static uint8_t g_comp[0x600];
static void glue_register_impl(void) { g_glueRegisterCalls++; }
static int g_entryReleases;
static uint8_t g_charList[3 * 0x120];
static void __fastcall charentry_release(uint8_t *e, void *unused)
{
	(void)unused;
	if (*(void **)(e + 0x110)) {
		g_entryReleases++;
		*(void **)(e + 0x110) = NULL; /* oyun: bileşeni yok eder, alanı sıfırlar; +0x114'ü bırakır ama sıfırlamaz */
	}
}
static void __fastcall framescript_execute(const char *code, const char *name)
{
	g_execCount++;
	g_execCode = code;
	snprintf(g_execName, sizeof g_execName, "%s", name ? name : "");
}
static void __fastcall create_apply(uint8_t *desc, int flag)
{
	g_createApplies++;
	g_createFlag = flag;
	g_createDescOk = desc != g_comp + 0x18 && !memcmp(desc, g_comp + 0x18, 0x16C);
}

static uint8_t g_moBuckets[4 * 12], *g_mo[3];

/* sahte DBC: kayıt = {id, sahip arşiv, metin*, ...}; CharSections adımı 40,
 * CharHairGeosets'in yeni yüklemesi bilerek farklı adımda (24 / canlı 32). */
static int g_dbFreed;
static void __stdcall smem_free(void *p, const char *n, int line, int flags) { (void)n; (void)line; (void)flags; free(p); g_dbFreed++; }
static int g_dbAllocs;
static void *__stdcall smem_alloc(uint32_t size, const char *n, int line, int flags) { (void)n; (void)line; (void)flags; g_dbAllocs++; return malloc(size); }
static int owner_of(const char *file)
{
	int out = -1;
	((int (__fastcall *)(void *, const char *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t))0x006549A0)(
	    0, file, 0, 0, (uint32_t)(uintptr_t)&out, 0, 0);
	return out;
}
static void fill_db(uint8_t *db, uint32_t stride, int owner, int nrec)
{
	uint8_t *rec = calloc(nrec, stride);
	char *str = malloc(32);
	snprintf(str, 32, "doku_%d.blp", owner);
	uint8_t **index = calloc(nrec + 1, sizeof *index);
	for (int i = 0; i < nrec; i++) {
		uint8_t *r = rec + i * stride;
		*(uint32_t *)r = i + 1;
		*(uint32_t *)(r + 4) = owner;
		*(char **)(r + 8) = str;
		index[i + 1] = r;
	}
	*(uint8_t **)(db + 0) = rec;
	*(uint32_t *)(db + 4) = nrec;
	*(uint8_t ***)(db + 8) = index;
	*(uint32_t *)(db + 0xC) = nrec;
	*(uint32_t *)(db + 0x10) = 1;
}
static int g_charSectionsLoads, g_cdiRecs = 3, g_cdiLoads;
static void __fastcall db_load_cdi(uint8_t *db, void *e)
{ (void)e; if (!*(uint32_t *)(db + 0x10)) { g_cdiLoads++; fill_db(db, 40, owner_of("DBFilesClient\\CreatureDisplayInfo.dbc"), g_cdiRecs); } }
static void __fastcall db_load_cmd(uint8_t *db, void *e)
{ (void)e; if (!*(uint32_t *)(db + 0x10)) fill_db(db, 24, owner_of("DBFilesClient\\CreatureModelData.dbc"), 3); }
static void __fastcall db_load_charsections(uint8_t *db, void *e)
{ (void)e; g_charSectionsLoads++; if (!*(uint32_t *)(db + 0x10)) fill_db(db, 40, 9, 3); }

static int g_lockDefaults, g_asyncDefaults;
static void __stdcall mock_lock_default(int x) { (void)x; g_lockDefaults++; }
static void __fastcall mock_async_default(void *list, void *tex) { (void)list; (void)tex; g_asyncDefaults++; }
static int g_componentInits;
static void component_init(void) { g_componentInits++; }
static uint8_t *g_tex[10];
static int g_imgFreed, g_imgFreedW;
static int __fastcall img_decode(uint8_t *tex, void *e)
{
	(void)e;
	void **fake = *(void ***)(tex + 0x138);
	const char *buf = fake ? fake[1] : NULL;
	if (!buf || strncmp(buf, "ARC=", 4) != 0)
		return 0;
	int arc = atoi(buf + 4);
	*(int *)(tex + 0x144) = 64 << arc;
	*(int *)(tex + 0x148) = 64 << arc;
	*(void **)(tex + 0x120) = malloc(16);
	*(int *)(tex + 0x11C) = arc; /* sahte: hangi dosyadan çözüldü */
	int req = *(int *)(tex + 0x118);
	*(int *)(tex + 0x1F0) = req;  /* sahte: çözücünün gördüğü istek */
	if (req == 8)
		*(int *)(tex + 0x118) = 4; /* gerçek çözücü gibi: otomatik istek seçilen biçimle değişir */
	else if (req == 9)
		*(int *)(tex + 0x118) = 5;
	return 1;
}
static void __fastcall img_free(void *bits, int fmt, int w, int h) { (void)fmt; (void)h; free(bits); g_imgFreed++; g_imgFreedW = w; }
static uint8_t *make_tex(const char *name, int arc, int loading)
{
	uint8_t *t = calloc(1, 0x200);
	strcpy((char *)t + 0x0C, name);
	*(uint32_t *)(t + 0x110) = 3; /* alfa yok + bayrak */
	*(uint32_t *)(t + 0x150) = 0x41;
	if (arc >= 0) {
		uint8_t *gx = new_gx(arc);
		*(uintptr_t *)(gx + 0x44) = 0x00448840; /* standart BLP geri çağırması */
		*(uint8_t **)(gx + 0x40) = t;
		*(uint8_t **)(t + 0x140) = gx;
	}
	if (loading)
		*(void **)(t + 0x138) = (void *)0x1234;
	return t;
}
static int tex_arc(int i) { return gx_arc(*(uint8_t **)(g_tex[i] + 0x140)); }

static void build_textures(void)
{
	/* arc indeksleri: 0 Z, 1 T, 2 C, 3 patch-5, 4 patch.MPQ, 5 model */
	g_tex[0] = make_tex("Creature\\Skin.blp", 1, 0);   /* T'den */
	g_tex[1] = make_tex("Item\\Sword.blp", 1, 0);       /* T'den */
	g_tex[2] = make_tex("Creature\\NewWolf.m2", 2, 0);  /* yalnız C */
	g_tex[3] = make_tex("", 4, 0);                        /* adsız (kodla üretilmiş) */
	g_tex[4] = make_tex("Creature\\Skin.blp", 1, 1);   /* hâlâ yükleniyor */
	g_tex[5] = make_tex("Item\\Sword.blp", -1, 0);      /* GxTex'i yok */
	g_tex[6] = make_tex("Creature\\Skin.blp", 1, 0);   /* karakter bileşeni: kendi geri çağırması */
	*(uintptr_t *)(*(uint8_t **)(g_tex[6] + 0x140) + 0x44) = 0x00449ABC;
	for (int k = 7; k <= 9; k++) {                        /* bellekte çözülmüş görüntüler (GxTex yok) */
		g_tex[k] = make_tex("Creature\\Skin.blp", -1, 0);
		*(void **)(g_tex[k] + 0x120) = malloc(16);
		*(int *)(g_tex[k] + 0x144) = 64 << 1, *(int *)(g_tex[k] + 0x148) = 64 << 1;
		*(int *)(g_tex[k] + 0x11C) = 1;
	}
	*(int *)(g_tex[7] + 0x118) = 2;                       /* sabit biçim: yeniden çözülür */
	*(int *)(g_tex[8] + 0x118) = 4;                       /* 8 isteğinden seçilmiş biçim */
	*(int *)(g_tex[9] + 0x118) = 5;                       /* 9 isteğinden seçilmiş (alfasız ten) */
	*(uint32_t *)0x00905820 = TEX_LINK;
	*(uintptr_t *)0x00905828 = (uintptr_t)g_tex[0];
	for (int i = 0; i < 10; i++)
		*(uintptr_t *)(g_tex[i] + TEX_LINK + 4) = i + 1 < 10 ? (uintptr_t)g_tex[i + 1] : 1;
}

/* --- sahte Lua ------------------------------------------------------------- */
typedef struct {
	int type; /* 0 nil, 1 bool, 3 number, 4 string */
	double n;
	char s[80];
} LV;
typedef struct {
	LV v[64];
	int top;
} FakeL;

static int __fastcall l_gettop(FakeL *L) { return L->top; }
static double __fastcall l_tonumber(FakeL *L, int i) { return L->v[i - 1].type == 3 ? L->v[i - 1].n : 0; }
static const char *__fastcall l_tostring(FakeL *L, int i) { return L->v[i - 1].type == 4 ? L->v[i - 1].s : NULL; }
static void __fastcall l_pushnil(FakeL *L) { L->v[L->top++].type = 0; }
static void __fastcall l_pushnumber(FakeL *L, double n) { L->v[L->top].type = 3; L->v[L->top++].n = n; }
static void __fastcall l_pushstring(FakeL *L, const char *s)
{
	L->v[L->top].type = 4;
	strncpy(L->v[L->top].s, s, sizeof L->v[0].s - 1);
	L->top++;
}
static void __fastcall l_pushboolean(FakeL *L, int b) { L->v[L->top].type = 1; L->v[L->top++].n = b != 0; }

typedef int (__fastcall *LuaCF)(FakeL *L);
static struct { char name[32]; LuaCF fn; } g_reg[32];
static int g_nreg, g_loadScriptCalls;

static void __fastcall reg_impl(const char *name, void *fn)
{
	strncpy(g_reg[g_nreg].name, name, 31);
	g_reg[g_nreg++].fn = (LuaCF)fn;
}

static void load_script_impl(void) { g_loadScriptCalls++; }

static LuaCF lua_fn(const char *name)
{
	for (int i = 0; i < g_nreg; i++)
		if (strcmp(g_reg[i].name, name) == 0)
			return g_reg[i].fn;
	return NULL;
}

/* --- sahte CGxDevice --------------------------------------------------------- */
static int g_setMipCalls, g_setMipValue = -1;
static void __fastcall dev_setmip_impl(void *dev, void *edx_unused, int mip)
{
	(void)edx_unused;
	g_setMipCalls++;
	g_setMipValue = mip;
	*(int *)((char *)dev + 0x31C) = mip;
}
/* thiscall -> fastcall köprüsü: ecx=this korunur, edx boş, yığında mip; ret 4 */

/* --- düzeneği kur ------------------------------------------------------------- */
static uint8_t *g_dev;

static void build_world(void)
{
	/* .text sayfaları */
	commit_at(0x0048FE00, 0x1000);
	commit_at(0x00490200, 0x100);
	commit_at(0x00654900, 0x200);
	commit_at(0x006F3000, 0x1000);
	commit_at(0x00704100, 0x100);
	/* .rdata: sürüm metni */
	commit_at(0x00837C00, 0x100);
	strcpy((char *)0x00837C10, "WoW [Release] Build 5875 (Sep 19 2006 20:32:39)");
	/* .data */
	commit_at(0x00C0ED00, 0x100);
	commit_at(0x00C53F00, 0x200);
	/* realmList / realmName CVar işaretçileri (oyun açılırken kaydeder) */
	commit_at(0x00C28100, 0x100);
	*(void **)0x00C2812C = NULL;
	*(void **)0x00C28130 = NULL;

	/* Storm arama: gerçek ilk 9 bayt, 0x6549A9'da çerçeveyi geri alıp C taklidine atla */
	static const uint8_t prologue[9] = {0x55, 0x8B, 0xEC, 0x81, 0xEC, 0x44, 0x02, 0x00, 0x00};
	put(0x006549A0, prologue, 9);
	static const uint8_t unwind[3] = {0x8B, 0xE5, 0x5D}; /* mov esp,ebp ; pop ebp */
	put(0x006549A9, unwind, 3);
	put_jmp(0x006549AC, (void *)real_search);

	/* doku sistemi */
	commit_at(0x00905800, 0x100);
	commit_at(0x00CEEFB0, 0x1100);
	commit_at(0x00B41400, 0x20);
	commit_at(0x0060ABE0, 0x10);
	put_jmp(0x0060ABE0, (void *)unit_rebuild);
	commit_at(0x00C7B290, 0x10);
	commit_at(0x00707350, 0x10);
	commit_at(0x007103A0, 0x10);
	commit_at(0x007133A0, 0x10);
	commit_at(0x007134B0, 0x10);
	commit_at(0x00695100, 0x10);
	commit_at(0x00613CF0, 0x10);
	put_jmp(0x00707350, (void *)scene_create);
	put_jmp(0x007103A0, (void *)m_release);

	put_jmp(0x007133A0, (void *)m_setcb);
	put_jmp(0x007134B0, (void *)m_setlight);
	put_jmp(0x00695100, (void *)doodad_create);
	put_jmp(0x00613CF0, (void *)obj_setmodel);
	commit_at(0x006A6710, 0x10);
	put_jmp(0x006A6710, (void *)map_unload_all);
	commit_at(0x00646430, 0x10);
	commit_at(0x006462E0, 0x10);
	put_jmp(0x006462E0, (void *)smem_alloc);
	commit_at(0x005540A0, 0x10);
	commit_at(0x00542E90, 0x10);
	commit_at(0x00543300, 0x10);
	commit_at(0x00C0DE40, 0x140);
	put_jmp(0x00646430, (void *)smem_free);
	commit_at(0x00449840, 0x10);
	commit_at(0x0044B110, 0x10);
	put_jmp(0x00449840, (void *)img_decode);
	put_jmp(0x0044B110, (void *)img_free);
	put_jmp(0x005540A0, (void *)db_load_charsections);
	put_jmp(0x00542E90, (void *)db_load_cdi);
	put_jmp(0x00543300, (void *)db_load_cmd);
	commit_at(0x00C96080, 0x10);
	commit_at(0x00C62950, 0x10);
	commit_at(0x00CA8050, 0x10);
	commit_at(0x0044A560, 0x10);
	commit_at(0x00594CE0, 0x10);
	commit_at(0x00594B90, 0x10);
	commit_at(0x00448670, 0x10);
	commit_at(0x006477C0, 0x10);
	commit_at(0x006487F0, 0x10);
	commit_at(0x00648460, 0x10);
	commit_at(0x00648730, 0x10);
	put_jmp(0x0044A560, (void *)tex_process);
	put_jmp(0x00594CE0, (void *)dev_texsize);
	put_jmp(0x00594B90, (void *)dev_texdirty);
	put_jmp(0x00448670, (void *)gx_release);
	put_jmp(0x006477C0, (void *)sf2_open);
	put_jmp(0x006487F0, (void *)sf2_size);
	put_jmp(0x00648460, (void *)sf2_read);
	put_jmp(0x00648730, (void *)sf2_close);
	/* dünya karesi (0x707490) -> call 0x706870 @ 0x70749C */
	commit_at(0x00707490, 0x20);
	commit_at(0x00706870, 0x10);
	commit_at(0x004439B0, 0x10);
	commit_at(0x0071D330, 0x10);
	put_jmp(0x00706870, (void *)m2cache_clean);
	put_jmp(0x004439B0, (void *)async_cancel);
	put_jmp(0x0071D330, (void *)m2shared_dtor);
	*(uint8_t *)0x0070749C = 0xE8;
	*(int32_t *)0x0070749D = (int32_t)(0x00706870 - (0x0070749C + 5));
	build_textures();

	/* SFile */
	commit_at(0x00653900, 0x10);
	commit_at(0x006553C0, 0x10);
	commit_at(0x00656690, 0x10);
	put_jmp(0x00656690, (void *)sf_open);
	put_jmp(0x006553C0, (void *)sf_size);
	put_jmp(0x00653900, (void *)sf_close);
	commit_at(0x00656B40, 0x10);
	put_jmp(0x00656B40, (void *)sf_read);

	/* Lua API */
	put_jmp(0x006F3070, (void *)l_gettop);
	put_jmp(0x006F3620, (void *)l_tonumber);
	put_jmp(0x006F3690, (void *)l_tostring);
	put_jmp(0x006F37F0, (void *)l_pushnil);
	put_jmp(0x006F3810, (void *)l_pushnumber);
	put_jmp(0x006F3890, (void *)l_pushstring);
	put_jmp(0x006F39F0, (void *)l_pushboolean);

	/* FrameScript_RegisterFunction: gerçek ilk 4 bayt (push esi; push edi; mov edi,ecx) */
	static const uint8_t regPro[4] = {0x56, 0x57, 0x8B, 0xF9};
	put(0x00704120, regPro, 4);
	static const uint8_t regUnwind[2] = {0x5F, 0x5E}; /* pop edi ; pop esi (ecx, edx korunur) */
	put(0x00704124, regUnwind, 2);
	put_jmp(0x00704126, (void *)reg_impl);

	/* teşhis çağrı noktaları: call varsayılan ; ret (4) */
	commit_at(0x004488F0, 0x20);
	commit_at(0x0044A510, 0x10);
	commit_at(0x00448920, 0x10);
	commit_at(0x0044A900, 0x10);
	{
		uint8_t c1[8] = {0x6A, 0x01, 0xE8, 0, 0, 0, 0, 0xC3}; /* oyundaki gibi: push 1 ; call ; ret */
		*(int32_t *)(c1 + 3) = (int32_t)(0x00448920 - (0x004488FA + 5));
		put(0x004488F8, c1, 8);
		put_jmp(0x00448920, (void *)mock_lock_default);
		uint8_t c2[6] = {0xE8, 0, 0, 0, 0, 0xC3};
		*(int32_t *)(c2 + 1) = (int32_t)(0x0044A900 - (0x0044A513 + 5));
		put(0x0044A513, c2, 6);
		put_jmp(0x0044A900, (void *)mock_async_default);
	}

	/* CCharacterComponent::Initialize çağrı noktası: call 0x476020 ; ret */
	commit_at(0x00402B00, 0x10);
	commit_at(0x00476000, 0x40);
	{
		uint8_t c[6] = {0xE8, 0, 0, 0, 0, 0xC3};
		*(int32_t *)(c + 1) = (int32_t)(0x00476020 - (0x00402B07 + 5));
		put(0x00402B07, c, 6);
		put_jmp(0x00476020, (void *)component_init);
	}

	/* glue betik fonksiyonları çağrı noktası: call 0x46ABB0 ; ret */
	commit_at(0x0046A800, 0x100);
	commit_at(0x0046AB00, 0x100);
	{
		uint8_t c[6] = {0xE8, 0, 0, 0, 0, 0xC3};
		*(int32_t *)(c + 1) = (int32_t)(0x0046ABB0 - (0x0046A880 + 5));
		put(0x0046A880, c, 6);
		put_jmp(0x0046ABB0, (void *)glue_register_impl);
	}
	commit_at(0x00704CD0, 0x10);
	put_jmp(0x00704CD0, (void *)framescript_execute);
	commit_at(0x00470B70, 0x10);
	put_jmp(0x00470B70, (void *)create_apply);
	commit_at(0x00473700, 0x10);
	put_jmp(0x00473700, (void *)charentry_release);
	commit_at(0x00B42000, 0x100);
	*(void **)0x00B42078 = NULL;
	*(void **)0x00B420F0 = NULL;
	*(void **)0x00B42144 = NULL;
	*(uint32_t *)0x00B42140 = 0;

	/* ten doku önbelleği: 5 çağrı noktası (call 0x47B150) */
	commit_at(0x00476E00, 0x400);
	commit_at(0x0047B150, 0x10);
	{
		static const uintptr_t sites[] = {0x00476EF2u, 0x00476FA4u, 0x00477025u, 0x004770B2u, 0x0047712Du};
		for (int i = 0; i < 5; i++) {
			uint8_t c[5] = {0xE8};
			*(int32_t *)(c + 1) = (int32_t)(0x0047B150 - (sites[i] + 5));
			put(sites[i], c, 5);
		}
		put_jmp(0x0047B150, (void *)texcache_getinfo);
	}

	/* ten parçası yapıştırma: 2 çağrı noktası (call 0x477460 ; ret) */
	commit_at(0x00477460, 0x10);
	{
		static const uintptr_t sites[] = {0x004770D7u, 0x0047715Bu};
		for (int i = 0; i < 2; i++) {
			uint8_t c[6] = {0xE8, 0, 0, 0, 0, 0xC3};
			*(int32_t *)(c + 1) = (int32_t)(0x00477460 - (sites[i] + 5));
			put(sites[i], c, 6);
		}
		put_jmp(0x00477460, (void *)mock_paste);
	}

	/* eşzamanlı doku yükleme içindeki CBLPFile::Open çağrısı: call 0x5A7B40 ; ret 8 */
	commit_at(0x004489F0, 0x10);
	commit_at(0x005A7B40, 0x10);
	{
		uint8_t c[8] = {0xE8, 0, 0, 0, 0, 0xC2, 0x08, 0x00};
		*(int32_t *)(c + 1) = (int32_t)(0x005A7B40 - (0x004489F6 + 5));
		put(0x004489F6, c, 8);
		put_jmp(0x005A7B40, (void *)blp_open);
	}

	/* LoadScriptFunctions çağrı noktası: call 0x490250 ; ret */
	uint8_t call[6] = {0xE8, 0, 0, 0, 0, 0xC3};
	*(int32_t *)(call + 1) = (int32_t)(0x00490250 - (0x0048FE9C + 5));
	put(0x0048FE9C, call, 6);
	put_jmp(0x00490250, (void *)load_script_impl);

	/* arşiv listesi */
	InitializeCriticalSection((LPCRITICAL_SECTION)0x00C54008);
	for (int i = 0; i < N_ARC; i++) {
		g_arc[i] = VirtualAlloc(NULL, 0x400, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
		strcpy((char *)g_arc[i] + 0x3C, ARCS[i].path);
		*(int *)(g_arc[i] + 0x148) = ARCS[i].prio;
	}
	for (int k = 0; k < N_ARC; k++)
		*(uintptr_t *)(g_arc[ORDER[k]] + 4) = k + 1 < N_ARC ? (uintptr_t)g_arc[ORDER[k + 1]] : 1; /* alt bit: son */
	*(uintptr_t *)0x00C53FE0 = (uintptr_t)g_arc[ORDER[0]];

	/* CGxDevice: vtable[11] -> thiscall köprüsü */
	g_dev = VirtualAlloc(NULL, 0x4000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
	void **vt = (void **)(g_dev + 0x800);
	uint8_t *thunk = g_dev + 0x900; /* thiscall(ecx=this, [esp+4]=mip) == fastcall(ecx, edx, [esp+4]) */
	put_jmp((uintptr_t)thunk, (void *)dev_setmip_impl);
	for (int i = 0; i < 32; i++)
		vt[i] = NULL;
	vt[11] = thunk;
	*(void ***)g_dev = vt;
	*(int *)(g_dev + 0x31C) = 1;
	*(void **)(g_dev + 0x3A68) = g_d3dStub; /* yer tutucu D3D doku */
	*(uint32_t *)(g_dev + 0x88) = 0;        /* doku belleği sayacı */
	*(void **)0x00C0ED38 = g_dev;
}

/* --- testler --------------------------------------------------------------------- */
typedef int (__fastcall *Search_t)(void *, const char *, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
#define SEARCH ((Search_t)0x006549A0)

static int served(const char *name)
{
	int out = -1;
	g_lastServed = -2;
	int r = SEARCH(0, name, 0x11, 0x22, (uint32_t)(uintptr_t)&out, 0x44, 0x55);
	return r == 1 ? out : r == 3 ? -3 : -1;
}

static int call_lua(const char *fn, FakeL *L)
{
	LuaCF f = lua_fn(fn);
	if (!f)
		return -1;
	int base = L->top;
	int n = f(L);
	(void)base;
	return n;
}

static void lua_reset(FakeL *L) { memset(L, 0, sizeof *L); }
static void push_s(FakeL *L, const char *s) { L->v[L->top].type = 4; strcpy(L->v[L->top++].s, s); }
static void push_n(FakeL *L, double n) { L->v[L->top].type = 3; L->v[L->top++].n = n; }

static int set_pack(const char *letter, int on)
{
	FakeL L;
	lua_reset(&L);
	push_s(&L, letter);
	push_n(&L, on);
	int n = call_lua("HDPack_Set", &L);
	return n >= 1 && L.v[2].type == 1 && L.v[2].n == 1;
}

static void nested_refresh(void)
{
	FakeL L;
	lua_reset(&L);
	call_lua("HDPack_Refresh", &L);
}

/* seçimi uygular (oyunda eklenti Set'ten 0,3 sn sonra Refresh çağırır) */
static void apply_packs(void)
{
	FakeL L;
	lua_reset(&L);
	call_lua("HDPack_Refresh", &L);
}

/* oyun klasöründeki sunucu ayarları (DLL'in klasörü = oyun klasörü burada) */
static char g_gameDir[MAX_PATH];
static void write_server(const char *realmlist, const char *realmName)
{
	char path[MAX_PATH + 32];
	snprintf(path, sizeof path, "%s\\realmlist.wtf", g_gameDir);
	FILE *f = fopen(path, "w");
	if (f) {
		if (realmlist)
			fprintf(f, "set realmlist \"%s\"\n", realmlist);
		fclose(f);
	}
	CreateDirectoryA(strcat(strcpy(path, g_gameDir), "\\WTF"), NULL);
	strcat(path, "\\Config.wtf");
	f = fopen(path, "w");
	if (f) {
		fprintf(f, "SET gxResolution \"1024x768\"\n");
		if (realmName)
			fprintf(f, "SET realmName \"%s\"\n", realmName);
		fclose(f);
	}
}

/* CVar taklidi: +0x20 değer */
static uint8_t g_cvarList[0x40], g_cvarName[0x40];
static void set_cvar(uintptr_t global, uint8_t *cv, const char *value)
{
	*(const char **)(cv + 0x20) = value;
	*(uint8_t **)global = value ? cv : NULL;
}

static void read_file(const char *path, char *buf, size_t size)
{
	buf[0] = 0;
	FILE *f = fopen(path, "rb");
	if (!f)
		return;
	size_t n = fread(buf, 1, size - 1, f);
	buf[n] = 0;
	fclose(f);
}

int main(int argc, char **argv)
{
	const char *dll = argc > 1 ? argv[1] : "HDToggle.dll";
	char dir[MAX_PATH], ini[MAX_PATH + 32], log[MAX_PATH + 32], buf[65536];
	GetFullPathNameA(dll, sizeof dir, dir, NULL);
	*strrchr(dir, '\\') = 0;
	snprintf(ini, sizeof ini, "%s\\HDToggle.ini", dir);
	snprintf(log, sizeof log, "%s\\HDToggle.log", dir);
	strcpy(g_gameDir, dir);
	write_server("play.octowow.st", "N'Zoth");

	build_world();

	printf("== kurulum (önceki ayar: C kapalı)\n");
	DeleteFileA(ini);
	WritePrivateProfileStringA("HDToggle", "off", "C", ini);
	WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini); /* testte gerçek ağa çıkma */
	HMODULE h = LoadLibraryA(dll);
	CHECK(h != NULL, "DLL yüklendi");
	if (!h)
		return 1;
	CHECK(*(uint8_t *)0x006549A0 == 0xE9, "Storm aramasına kanca kuruldu");
	CHECK(*(uint8_t *)0x0048FE9C == 0xE8 && *(int32_t *)0x0048FE9D != (int32_t)(0x00490250 - 0x0048FEA1),
	      "LoadScriptFunctions çağrı noktası yönlendirildi");
	read_file(log, buf, sizeof buf);
	CHECK(strstr(buf, "ready") && strstr(buf, "packs off: C"), "günlük: hazır, ayardaki C kapalı");

	printf("== arayüz yüklemesi\n");
	((void (*)(void))0x0048FE9C)();
	CHECK(g_loadScriptCalls == 1, "asıl LoadScriptFunctions çağrıldı");
	CHECK(lua_fn("HDPack_List") && lua_fn("HDPack_Set") && lua_fn("HDPack_Refresh") && lua_fn("HDPack_Info"),
	      "4 Lua fonksiyonu kaydedildi");
	((void (*)(void))0x0048FE9C)(); /* /reload */
	CHECK(g_loadScriptCalls == 2 && g_nreg == 14, "/reload sonrası yeniden kaydedildi");

	printf("== dosya yönlendirme\n");
	/* C kapalı (ayardan) */
	CHECK(served("Creature\\Wolf.m2") == 3, "C kapalı: Wolf.m2 bir alttaki açık arşivden (patch-5)");
	CHECK(served("Creature\\NewWolf.m2") == 2, "C kapalı: yalnızca C'de olan dosya yine C'den");
	CHECK(served("Creature\\Skin.blp") == 1, "C kapalı: üstteki açık T'nin dosyası değişmez");
	CHECK(served("Item\\Sword.blp") == 1, "C ile ilgisiz dosya değişmez");
	CHECK(served("Yok\\Boyle.blp") == -1, "olmayan dosya bulunamaz");
	CHECK(served("Item\\Old.blp") == -3, "açık T'nin sildiği dosya silinmiş kalır");
	CHECK(served("Creature\\Gone.m2") == -3,
	      "C kapalı: C'deki dosya yerine alttaki patch-5'in silme işareti geçerli (paket yokmuş gibi)");
	served("Creature\\Wolf.m2");
	CHECK(g_lastArgs[0] == 0x11 && g_lastArgs[1] == 0x22 && g_lastArgs[3] == 0x44 && g_lastArgs[4] == 0x55,
	      "yönlendirmede çağıranın argümanları birebir taşınır");

	CHECK(set_pack("C", 1), "HDPack_Set(\"C\", 1)");
	CHECK(served("Creature\\Wolf.m2") == 3,
	      "Set tek başına dosya çözmeyi değiştirmez (tablolar yenilenmeden dosyalar yeni seçime geçmez)");
	apply_packs();
	CHECK(served("Creature\\Wolf.m2") == 2, "C açık: Wolf.m2 yine C'den");
	CHECK(set_pack("T", 0) && set_pack("C", 0), "T ve C kapatıldı");
	apply_packs();
	CHECK(served("Creature\\Skin.blp") == 4, "T ve C kapalı: Skin.blp iki kapalıyı atlayıp patch.MPQ'dan");
	CHECK(served("Item\\Sword.blp") == 4, "T kapalı: Sword.blp patch.MPQ'dan");
	CHECK(served("Item\\Old.blp") == 5, "T kapalı: T'nin silme işareti de devre dışı, alttaki dosya görünür");
	CHECK(!set_pack("Z", 0), "korumalı Z kapatılamaz");
	CHECK(!set_pack("CC", 0) && !set_pack("1", 0), "geçersiz harf reddedilir");

	/* belirli arşivden arama (arc != 0) asla değişmez */
	int out = -1;
	SEARCH(g_arc[2], "Creature\\Wolf.m2", 0, 0, (uint32_t)(uintptr_t)&out, 0, 0);
	CHECK(out == 2, "belirli arşivde arama kancadan etkilenmez");

	printf("== HDPack_List / Info / Refresh\n");
	FakeL L;
	lua_reset(&L);
	int n = call_lua("HDPack_List", &L);
	CHECK(n == 8, "List: Z gizli, C, H, T ve V var (4 paket x 2 değer)");
	CHECK(n == 8 && !strcmp(L.v[0].s, "C") && L.v[1].type == 1 && L.v[1].n == 0 &&
	          !strcmp(L.v[2].s, "H") && L.v[3].n == 1 && !strcmp(L.v[4].s, "T") && L.v[5].n == 0 &&
	          !strcmp(L.v[6].s, "V") && L.v[7].n == 1,
	      "List: harf sırasıyla, C ve T kapalı, H ve V açık");
	lua_reset(&L);
	n = call_lua("HDPack_Info", &L);
	CHECK(n == 4 && !strcmp(L.v[0].s, "1.2.7") && L.v[1].n == 1 && !strcmp(L.v[2].s, "ready") && L.v[3].n == 0,
	      "Info: sürüm, tamam, durum, kilitli değil");

	printf("== doku yeniden kurma (HDPack_Refresh)\n");
	set_pack("C", 1); set_pack("T", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L); /* uygulanan seçim = hepsi açık */
	build_textures();
	g_released = g_processed = g_d3dReleased = g_dirtyCalls = 0;
	g_imgFreed = 0;
	lua_reset(&L);
	n = call_lua("HDPack_Refresh", &L);
	CHECK(n == 1 && L.v[0].type == 3 && L.v[0].n == 0 && g_processed == 0, "değişiklik yokken hiçbir doku kurulmaz");
	set_pack("T", 0);
	uint8_t *gxSkin = *(uint8_t **)(g_tex[0] + 0x140), *gxSword = *(uint8_t **)(g_tex[1] + 0x140);
	uint32_t memBefore = *(uint32_t *)(g_dev + 0x88);
	lua_reset(&L);
	n = call_lua("HDPack_Refresh", &L);
	CHECK(n == 1 && L.v[0].n == 2, "T kapatınca 2 doku yeniden kuruldu");
	CHECK(*(uint8_t **)(g_tex[0] + 0x140) == gxSkin && *(uint8_t **)(g_tex[1] + 0x140) == gxSword,
	      "GxTex nesnelerinin adresi değişmez (saklanan işaretçiler geçerli kalır)");
	CHECK(tex_arc(0) == 2 && tex_arc(1) == 4, "Skin.blp C'nin, Sword.blp patch.MPQ'nun biçimiyle yeniden tanımlandı");
	CHECK(*(int32_t *)(gxSkin + 0x24) == (64 << 2) && *(int32_t *)(gxSkin + 0x28) == (32 << 2) &&
	          *(int32_t *)(gxSkin + 0x3c) == 0x102,
	      "yeni genişlik, yükseklik ve bayrak eski nesneye aktarıldı");
	CHECK(gxSkin[1] == 1 && gxSkin[2] == 1 && *(void **)(gxSkin + 0x48) == g_d3dStub,
	      "D3D dokusu yer tutucuyla değişti, yeniden oluşturma işaretlendi");
	CHECK(*(int32_t *)(gxSkin + 0x0c) == (32 << 2) && *(int32_t *)(gxSkin + 0x10) == (64 << 2) &&
	          *(int16_t *)(gxSkin + 0x1a) == -1,
	      "güncelleme bölgesi yeni boyuta ayarlandı");
	CHECK(g_d3dReleased == 2, "eski D3D dokuları bırakıldı");
	CHECK(g_released == 2 && g_lastReleased != gxSkin && g_lastReleased != gxSword, "geçici GxTex'ler iade edildi, eskiler değil");
	CHECK(g_dirtyCalls == 2 && g_lastDirty == gxSword && g_dirtyImmediate == 0 && !g_lastDirtyRect[0] &&
	          !g_lastDirtyRect[1] && !g_lastDirtyRect[2] && !g_lastDirtyRect[3],
	      "tüm doku kirli işaretlendi (tembel, oyunun doku kalitesi yolu gibi)");
	CHECK(*(uint32_t *)(g_dev + 0x88) - memBefore == (uint32_t)((256 * 128 - 128 * 64) + (1024 * 512 - 128 * 64)),
	      "aygıtın doku belleği sayacı güncellendi");
	CHECK(tex_arc(2) == 2 && tex_arc(3) == 4, "etkilenmeyen ve adsız dokulara dokunulmaz");
	CHECK(*(void **)(g_tex[4] + 0x138) == (void *)0x1234 && tex_arc(4) == 1, "yüklenmekte olan dokuya dokunulmaz");
	CHECK(*(void **)(g_tex[5] + 0x140) == NULL, "GxTex'i olmayan dokuya dokunulmaz");
	CHECK(tex_arc(6) == 1 && *(uint32_t *)(g_tex[6] + 0x110) == 3,
	      "kendi geri çağırması olan (karakter/saç) dokuya dokunulmaz");
	CHECK(*(int *)(g_tex[7] + 0x11C) == 2 && *(int *)(g_tex[7] + 0x144) == (64 << 2) && *(void **)(g_tex[7] + 0x138) == NULL,
	      "bellekteki parça dokusu yeni dosyadan (C) yeniden çözüldü");
	CHECK(g_imgFreed == 3 && g_imgFreedW == (64 << 1), "eski görüntüler eski boyutlarıyla bırakıldı");
	CHECK(*(int *)(g_tex[8] + 0x11C) == 2 && *(int *)(g_tex[8] + 0x1F0) == 8 && *(int *)(g_tex[8] + 0x118) == 4,
	      "çözücünün seçtiği biçim (4) de yeniden çözülür, istek oyunun asıl isteği (8)");
	CHECK(*(int *)(g_tex[9] + 0x11C) == 2 && *(int *)(g_tex[9] + 0x1F0) == 9 && *(int *)(g_tex[9] + 0x144) == (64 << 2),
	      "alfasız ten parçası (5) yeni dosyadan çözülür, istek karakter önbelleğinin isteği (9): eski HD ten kalmaz");
	CHECK((g_flagsAtProcess & 1) == 0 && (*(uint32_t *)(g_tex[0] + 0x150) & 0x40) == 0,
	      "işlemeden önce alfa-yok ve 0x40 bayrakları temizlenir");
	CHECK(*(void **)(g_tex[0] + 0x138) == NULL && *(void **)(g_tex[1] + 0x138) == NULL, "yükleme nesnesi geri temizlenir");
	lua_reset(&L);
	call_lua("HDPack_Refresh", &L);
	CHECK(L.v[0].n == 0, "ikinci yenileme bir şey yapmaz");
	set_pack("T", 1); set_pack("C", 0);
	lua_reset(&L);
	call_lua("HDPack_Refresh", &L);
	CHECK(L.v[0].n == 2 && tex_arc(0) == 1 && tex_arc(1) == 1 && tex_arc(2) == 2,
	      "T açık C kapalı: ikisi T'ye döner, yalnız-C dosyası C'de kalır");
	set_pack("C", 1);
	lua_reset(&L);
	call_lua("HDPack_Refresh", &L);
	CHECK(L.v[0].n == 0, "C açılınca T'deki dosyalar değişmediği için kurulum yok");

	printf("== model önbelleği\n");
	set_pack("C", 1); set_pack("T", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	build_models();
	set_pack("C", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	uint8_t **slot7 = (uint8_t **)(0x00CEEFC4 + 7 * 4), **slot9 = (uint8_t **)(0x00CEEFC4 + 9 * 4);
	/* model zincirden ÇIKARILMAZ (zincir dışındaki model Release'te bekleyen okuma
	 * iptal edilmeden silinir, oyunda 0x71CD18 çökmesi); adı işaretlenir */
	CHECK(*slot7 == g_m2[0] && *(uint8_t **)(g_m2[0] + 0x128) == g_m2[1] && *slot9 == g_m2[3] &&
	          *(uint8_t ***)(g_m2[1] + 0x124) == (uint8_t **)(g_m2[0] + 0x128),
	      "zincir ve bağlar olduğu gibi kaldı (silme oyunun kendi yaşam döngüsünde)");
	CHECK(!strcmp((char *)g_m2[0] + 0x20, "\x01" "Creature\\Wolf") && *(int *)g_m2[0] == 1,
	      "C kapalı: Wolf'un adı aramaların eşleşemeyeceği biçimde işaretlendi, referans değişmedi");
	CHECK(!strcmp((char *)g_m2[1] + 0x20, "Creature\\NewWolf"), "yalnız-C NewWolf işaretlenmedi (dosyası değişmiyor)");
	CHECK(!strcmp((char *)g_m2[3] + 0x20, "\x01" "Creature\\Gone"), "Gone: alttaki silme işareti geçerli, işaretlendi");
	CHECK(!strcmp((char *)g_m2[2] + 0x20, "Item\\Sword"), "Sword etkilenmedi");
	set_pack("C", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(!strcmp((char *)g_m2[0] + 0x20, "\x01" "Creature\\Wolf"), "işaretli model ikinci kez işaretlenmez");
	{
		static uint8_t longName[0x164];
		memset(longName + 0x20, 'a', 0x103);
		longName[0x20 + 0x103] = 0;
		memcpy(longName + 0x20, "Creature\\", 9);
		*(uint8_t **)(0x00CEEFC4 + 11 * 4) = longName;
		set_pack("C", 0);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		CHECK(longName[0x20] == 'C' && longName[0x20 + 0x103] == 0, "adı tampona sığmayan model taşırılmadan atlanır");
		*(uint8_t **)(0x00CEEFC4 + 11 * 4) = NULL;
	}
	set_pack("C", 1); /* önceki durum */
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	set_pack("C", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	{
		/* LRU: A -> S1 (eskimiş, bekleyen okuma) -> B -> S2 (eskimiş, son) */
		uint8_t *cache = (uint8_t *)0x00CEEFB0, *lru[4];
		const char *nm[4] = {"Item\\Axe", "\x01" "Creature\\Old1", "Item\\Bow", "\x01" "Creature\\Old2"};
		static void *readObj[2] = {(void *)0x7171, 0};
		for (int i = 0; i < 4; i++) {
			lru[i] = calloc(1, 0x164);
			strcpy((char *)lru[i] + 0x20, nm[i]);
		}
		*(void **)(lru[1] + 0x0C) = readObj;
		uint8_t **slot = (uint8_t **)(cache + 0x0C);
		for (int i = 0; i < 4; i++) {
			*slot = lru[i];
			*(uint8_t ***)(lru[i] + 0x14) = slot;
			slot = (uint8_t **)(lru[i] + 0x18);
		}
		*(uint8_t ***)(cache + 0x10) = slot;
		CHECK(*(uint8_t *)0x0070749C == 0xE8 && 0x0070749C + 5 + *(int32_t *)0x0070749D != 0x00706870,
		      "model önbelleği temizleyicisinin çağrı noktasına kanca kuruldu");
		void (__fastcall *frame)(void *, void *) = (void (__fastcall *)(void *, void *))(0x0070749C + 5 + *(int32_t *)0x0070749D);
		int freed0 = g_dbFreed;
		g_m2Cleans = g_m2Dtors = g_asyncCancels = 0;
		uint8_t *b = lru[2], *a = lru[0];
		frame(cache, NULL);
		CHECK(*(uint8_t **)(cache + 0x0C) == a && *(uint8_t **)(a + 0x18) == b &&
		          *(uint8_t ***)(b + 0x14) == (uint8_t **)(a + 0x18) && *(uint8_t **)(b + 0x18) == NULL &&
		          *(uint8_t ***)(cache + 0x10) == (uint8_t **)(b + 0x18),
		      "eskimiş modeller LRU'dan çıkarıldı, kalanların bağları ve son yuva doğru");
		CHECK(g_m2Dtors == 2 && g_dbFreed - freed0 == 2, "eskimiş iki model oyunun yıkıcısıyla silinip SMemFree ile bırakıldı");
		CHECK(g_asyncCancels == 1 && g_lastCancelled == readObj && g_lastClosed == (void *)0x7171,
		      "bekleyen okuma iptal edildi ve dosyası kapatıldı (oyunun temizleyicisiyle aynı)");
		CHECK(g_m2Cleans == 1, "oyunun kendi temizleyicisi ardından çağrıldı");
		frame(cache, NULL);
		CHECK(g_m2Dtors == 2 && g_m2Cleans == 2 && *(uint8_t **)(cache + 0x0C) == a, "ikinci karede silinecek bir şey yok");
		*(uint8_t **)(cache + 0x0C) = NULL;
		*(uint8_t ***)(cache + 0x10) = (uint8_t **)(cache + 0x0C);
		free(a);
		free(b);
	}
	build_models();

	printf("== karakterleri yeniden kurma\n");
	*(void **)(g_scene + 4) = (void *)0x4444; /* kurulu sahne: model önbelleği var */
	*(uint8_t **)0x00C7B298 = g_scene;
	*(uint8_t **)(g_scene + 8) = NULL;
	build_objects();
	g_rebuilt = g_rebuildSawFakeOk = 0;
	set_pack("C", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_rebuilt == 2, "oyuncu ve NPC yeniden kuruldu, eşya ve kaydı olmayan atlandı");
	CHECK(g_rebuildSawFakeOk == 2, "kontrol için yalnız 0x0C alanı farklı birebir kopya gösterildi");
	CHECK(*(uint8_t **)(g_obj[0] + 0xB34) == g_rec[0] && *(uint8_t **)(g_obj[1] + 0xB34) == g_rec[1],
	      "yeniden kurulanlarda gerçek kayıt geri yazıldı");
	CHECK(*(uint8_t **)(g_obj[4] + 0xB34) == g_rec[1], "kurulamayan karakterde eski kayıt geri kondu");
	CHECK(*(void **)(g_obj[3] + 0xB34) == NULL, "kaydı olmayan karaktere dokunulmadı");
	g_rebuilt = 0;
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_rebuilt == 0, "değişiklik yoksa karakter kurulmaz");
	/* sahne var ama model önbelleği yok (yarı kurulu): model oluşturmak oyunda
	 * 0x706B8C'de çöküyordu; karakter ve nesne kurulmamalı */
	*(void **)(g_scene + 4) = NULL;
	g_rebuilt = 0;
	set_pack("C", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_rebuilt == 0, "sahne hazır değilken karakter/nesne kurulmaz (çökme yok)");
	read_file(log, buf, sizeof buf);
	CHECK(strstr(buf, "world scene not ready") != NULL, "atlandığı günlüğe yazıldı");
	*(void **)(g_scene + 4) = (void *)0x4444;
	/* dünya yüklüyken: karakterler dünya boşaltılmadan önce kurulur */
	*(uintptr_t *)0x00C96088 = 0x7000;
	g_step = g_unloadStep = g_lastRebuildStep = 0; g_rebuilt = 0; g_unloadAll = 0;
	set_pack("C", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_rebuilt == 2 && g_unloadAll == 1 && g_lastRebuildStep < g_unloadStep,
	      "karakterler dünya yeniden yüklenmeden önce, sahne geçerliyken kuruldu");
	*(uintptr_t *)0x00C96088 = 1;
	/* bir karakteri kurmak başka bir nesneyi listeden çıkarabilir (oyunda
	 * HDToggle+0x2998 çökmesi): çıkan nesne kullanılmadan atlanmalı */
	build_objects();
	g_rebuilt = 0; g_unlinkNpcOnRebuild = 1;
	set_pack("C", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_rebuilt == 1, "kurma sırasında listeden çıkan nesneye dokunulmadı (çökme yok)");
	read_file(log, buf, sizeof buf);
	CHECK(strstr(buf, "skipped (gone or changed during the rebuild)") != NULL, "atlanan nesne günlüğe yazıldı");
	/* kurma sırasında yenileme yeniden çağrılırsa iç içe çalışmaz */
	build_objects();
	g_rebuilt = 0; g_nestedRefreshOnRebuild = 1;
	set_pack("C", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	read_file(log, buf, sizeof buf);
	CHECK(strstr(buf, "nested call ignored") != NULL && g_rebuilt == 2, "iç içe yenileme reddedildi, dıştaki tamamlandı");
	*(uint8_t **)0x00B41414 = NULL;

	printf("== dekor objeleri ve oyun nesneleri\n");
	/* paylaşılan veriler: biri önbellekten çıkarılmış (bağları boş), biri zincirde */
	static uint8_t shStale[0x164], shLive[0x164], defA[0x200], defB[0x200], defC[0x200];
	strcpy((char *)shStale + 0x20, "\x01" "World\\Barrel"); /* eskimiş işaretli */
	strcpy((char *)shLive + 0x20, "World\\Fence");
	*(void **)(shLive + 0x124) = (void *)0x1111; /* zincirde */
	*(uint8_t **)0x00C7B298 = g_scene;
	*(uint8_t **)(g_scene + 8) = NULL;
	uint8_t *mA = mk_model(shStale, 0x006951E0, defA); *(uint8_t **)(defA + 0x88) = mA;  /* değişmiş dekor */
	uint8_t *mB = mk_model(shLive, 0x006951E0, defB);  *(uint8_t **)(defB + 0x88) = mB;  /* değişmemiş dekor */
	uint8_t *mC = mk_model(shStale, 0x00123456, defC); *(uint8_t **)(defC + 0x88) = mC;  /* dekor değil */
	(void)mB; (void)mC;
	build_objects();
	static uint8_t goDesc[16];
	*(uint32_t *)(goDesc + 8) = 0x21; /* oyun nesnesi */
	*(uint8_t **)(g_obj[2] + 8) = goDesc;
	*(void ***)g_obj[2] = g_goVt;
	g_goVt[0x28 / 4] = (void *)go_modelname;
	uint8_t *mGo = mk_model(shStale, 0, NULL);
	*(uint8_t **)(g_obj[2] + 0xD8) = mGo;
	g_modelReleased = g_cbCleared = g_lightCleared = g_doodadCreated = g_sceneCreated = g_setModel = 0;
	set_pack("C", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_doodadCreated == 1 && g_lastDoodadDef == defA && !strcmp(g_lastDoodadName, "World\\Barrel.m2"),
	      "yalnızca dosyası değişen dekor, aynı tanım ve doğru adla yeniden oluşturuldu");
	CHECK(*(uint8_t **)(defA + 0x88) != mA && *(uint8_t **)(defB + 0x88) == mB && *(uint8_t **)(defC + 0x88) == mC,
	      "değişmeyen dekora ve dekor olmayan modele dokunulmadı");
	CHECK(g_cbCleared >= 1 && g_lightCleared >= 1, "eski dekor modelinin geri çağırmaları temizlendi");
	CHECK(g_sceneCreated == 1 && !strcmp(g_lastSceneName, "World\\Chest.mdx") && g_setModel == 1 &&
	          g_lastSetModelObj == g_obj[2] && *(uint8_t **)(g_obj[2] + 0xD8) != mGo,
	      "oyun nesnesi kendi model adıyla yeni modele geçti");
	CHECK(g_modelReleased == 2, "eski dekor modeli ve yerel oyun nesnesi referansı bırakıldı (denge)");
	{
		/* çok sayıda oyun nesnesi: çağırma kuralı uyumsuzluğu her nesnede yığını
		 * kaydırıp yerel değişkenleri bozuyordu (oyunda 3 çökme) */
		enum { NGO = 40 };
		static uint8_t goDescMany[16], *gos[NGO];
		*(uint32_t *)(goDescMany + 8) = 0x21;
		for (int i = 0; i < NGO; i++) {
			gos[i] = calloc(1, 0x1000);
			*(uint8_t **)(gos[i] + 8) = goDescMany;
			*(void ***)gos[i] = g_goVt;
			*(uint8_t **)(gos[i] + 0xD8) = mk_model(shStale, 0, NULL);
		}
		for (int i = 0; i < NGO; i++)
			*(uintptr_t *)(gos[i] + 0x3C + 4) = i + 1 < NGO ? (uintptr_t)gos[i + 1] : 1;
		*(uint32_t *)(g_objmgr + 0xA4) = 0x3C;
		*(uint8_t **)(g_objmgr + 0xAC) = gos[0];
		*(uint8_t **)0x00B41414 = g_objmgr;
		g_setModel = 0;
		set_pack("C", 1);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		int all = 1;
		for (int i = 0; i < NGO; i++)
			all &= *(uint8_t **)(gos[i] + 0xD8) != NULL && !model_is_stale_mock(*(uint8_t **)(gos[i] + 0xD8));
		CHECK(g_setModel == NGO && all, "40 oyun nesnesinin hepsi yeni modele geçti (yığın dengede, yerel değişkenler sağlam)");
		set_pack("C", 0);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
	}
	*(uint8_t **)0x00B41414 = NULL;
	*(uint8_t **)0x00C7B298 = NULL;

	printf("== DBC tablolarını yerinde yeniden yükleme\n");
	set_pack("C", 1); set_pack("T", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	memset((void *)0x00C0DE40, 0, 0x140);
	fill_db((uint8_t *)0x00C0DE88, 40, 1, 4);   /* canlı CreatureDisplayInfo: T'den, 4 kayıt */
	fill_db((uint8_t *)0x00C0DE60, 32, 1, 3);   /* canlı CreatureModelData: adım 32 */
	fill_db((uint8_t *)0x00C0DF64, 40, 1, 3);   /* canlı CharSections (sabit tablo) */
	uint8_t **cidx = *(uint8_t ***)(0x00C0DE88 + 8);
	uint8_t *rec1 = cidx[1], *rec4 = cidx[4];
	g_dbFreed = 0; g_charSectionsLoads = 0;
	set_pack("T", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	cidx = *(uint8_t ***)(0x00C0DE88 + 8);
	CHECK(cidx[1] == rec1 && *(uint32_t *)(rec1 + 4) == 4 && !strcmp(*(char **)(rec1 + 8), "doku_4.blp"),
	      "T kapalı: CreatureDisplayInfo kayıtları aynı adreste, patch.MPQ içeriğiyle güncellendi");
	CHECK(*(uint32_t *)(cidx[2] + 4) == 4 && *(uint32_t *)(cidx[3] + 4) == 4, "tüm ortak kayıtlar güncellendi");
	CHECK(cidx[4] == rec4 && *(uint32_t *)(rec4 + 4) == 1, "yeni dosyada olmayan kayda dokunulmadı");
	CHECK(g_dbFreed == 0, "saklanan sürümün dizini ve metinleri serbest bırakılmadı");
	uint8_t **hidx = *(uint8_t ***)(0x00C0DE60 + 8);
	CHECK(*(uint32_t *)(hidx[1] + 4) == 1, "kayıt boyutu farklı tablo (32/24) güvenlik için atlandı");
	CHECK(g_charSectionsLoads == 0, "dosyası değişmeyen tablo (CharSections T'de yok) yüklenmez");
	g_cdiLoads = 0;
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_cdiLoads == 0, "değişiklik yoksa DBC yüklenmez");
	/* T'nin dosyasında canlı tabloda olmayan kimlikler var (oyun paketler kapalı
	 * açılmış gibi): eklenmezlerse onlara başvuran kayıt boş kayda çıkar (oyunda
	 * 0x60B0A9 çökmesi) */
	g_cdiRecs = 7; g_dbAllocs = 0;
	set_pack("T", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	cidx = *(uint8_t ***)(0x00C0DE88 + 8);
	CHECK(*(uint32_t *)(rec1 + 4) == 1 && cidx[1] == rec1, "T açılınca kayıtlar aynı adreste T içeriğine döndü");
	CHECK(*(uint32_t *)(0x00C0DE88 + 0xC) == 7 && cidx[5] && cidx[7] && *(uint32_t *)cidx[7] == 7 &&
	          *(uint32_t *)(cidx[6] + 4) == 1,
	      "yeni dosyadaki yeni kimlikler tabloya eklendi (dizin büyüdü)");
	CHECK(g_dbAllocs == 4, "bir yeni dizin ve eklenen 3 kayıt için ayrı bellek ayrıldı");
	uint8_t *rec7 = cidx[7];
	g_cdiLoads = 0; g_dbAllocs = 0; g_cdiRecs = 3;
	set_pack("T", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	cidx = *(uint8_t ***)(0x00C0DE88 + 8);
	CHECK(g_cdiLoads == 0 && *(uint32_t *)(rec1 + 4) == 4,
	      "T tekrar kapalı: saklanan patch.MPQ sürümü kullanıldı, dosya yeniden okunmadı (bellek sabit)");
	CHECK(cidx[7] == rec7 && *(uint32_t *)(rec7 + 4) == 1 && *(uint32_t *)(0x00C0DE88 + 0xC) == 7,
	      "eklenen ama yeni sürümde olmayan kayıt geçerli kaldı");
	set_pack("T", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_cdiLoads == 0 && g_dbAllocs == 0 && *(uint32_t *)(rec1 + 4) == 1 && *(uint32_t *)(rec7 + 4) == 1,
	      "T tekrar açık: saklanan T sürümü, yeni bellek ayrılmadan (eklenen kayıt yeni bloğu bozmadı)");
	for (int i = 0; i < 6; i++) {
		set_pack("T", i & 1);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
	}
	CHECK(g_cdiLoads == 0 && g_dbAllocs == 0, "art arda aç/kapat bellek ayırmaz, dosya okumaz");
	g_cdiRecs = 3;
	memset((void *)0x00C0DE40, 0, 0x140);
	set_pack("C", 0); /* sonraki bölümün beklediği durum (C kapalı, uygulanmış) */
	lua_reset(&L); call_lua("HDPack_Refresh", &L);

	printf("== karakter ten yerleşimi ve ölçeği (2x / 4x)\n");
	{
		static const uintptr_t lay256[] = {0x4778D3, 0x477930, 0x477935};
		static const uintptr_t lay512[] = {0x477CA1, 0x477DD1, 0x477F43};
		static const uintptr_t lay255[] = {0x477A10, 0x477A15};
		static const uintptr_t alloc256[] = {0x4760A8, 0x4760AD, 0x476792, 0x476797};
		static const uintptr_t pool256[] = {0x44AFBA, 0x44AFC6};
		commit_at(0x0044AF00, 0x100); commit_at(0x00476000, 0x2000); commit_at(0x00B42400, 0x100);
		/* oyunun doku havuzu boyut denetimi: cmp edx, imm32 / cmp edi, imm32 */
		*(uint8_t *)0x44AFB8 = 0x81; *(uint8_t *)0x44AFB9 = 0xFA;
		*(uint8_t *)0x44AFC4 = 0x81; *(uint8_t *)0x44AFC5 = 0xFF;
		int found = SEARCH(0, "VanillaHelpers\\ResizeCharacterSkin.txt", 0, 0, 0, 0, 0);
		CHECK(found == 1, "işaret dosyası gizlenmez (VanillaHelpers hep T'nin 2x'iyle açar)");
		set_pack("T", 0); /* açılışta T ve C kapalı (uygulanmış seçim) */
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		/* oyun henüz başlamadı: VanillaHelpers işaret dosyasına göre 2x uyguladı */
		for (int i = 0; i < 3; i++) *(uint32_t *)lay256[i] = 512;
		for (int i = 0; i < 3; i++) *(uint32_t *)lay512[i] = 1024;
		for (int i = 0; i < 2; i++) *(uint32_t *)lay255[i] = 511;
		for (int i = 0; i < 4; i++) *(uint32_t *)alloc256[i] = 512;
		for (int i = 0; i < 2; i++) *(uint32_t *)pool256[i] = 512; /* VanillaHelpers havuz sınırını da 2x yazar */
		for (int i = 0; i < 40; i++) ((uint32_t *)0x00B42450)[i] = (uint32_t)(i * 4 + 4) << 1;
		g_componentInits = 0;
		((void (*)(void))0x00402B07)(); /* oyunun birleştirme sistemini başlatması */
		CHECK(g_componentInits == 1, "asıl CCharacterComponent::Initialize çağrıldı");
		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "pack T: character skins 2x") && strstr(buf, "pack C: character skins 4x") &&
		      !strstr(buf, "pack Z: character"), "paketlerin ten ölçeği temel ten dokusundan ölçüldü (kapalı paketler de)");
		int ok = 1;
		for (int i = 0; i < 4; i++) ok &= *(uint32_t *)alloc256[i] == 1024;
		CHECK(ok, "C 4x ten getiriyor: tampon 4x ayrıldı (işaret dosyası 2x dese de)");
		CHECK(strstr(buf, "skin compositing buffer raised to 1024x1024 for 4x skins") != NULL, "günlüğe yazıldı");
		CHECK(*(uint32_t *)0x44AFBA == 256 && *(uint32_t *)0x44AFC6 == 256,
		      "doku havuzu boyut sınırı 256'ya döndü (büyük ten tamponu havuz tablosunun dışına indekslemez)");
		CHECK(*(uint8_t *)0x44AFB8 == 0x81 && *(uint8_t *)0x44AFB9 == 0xFA && *(uint8_t *)0x44AFC4 == 0x81 &&
		      *(uint8_t *)0x44AFC5 == 0xFF, "yalnız karşılaştırma değeri değişti, komut baytları aynı");
		CHECK(strstr(buf, "texture pool size limit at 0x0044AFBA restored to 256 (was 512)") &&
		      strstr(buf, "texture pool size limit at 0x0044AFC6 restored to 256 (was 512)"), "günlüğe yazıldı");
		ok = 1;
		for (int i = 0; i < 3; i++) ok &= *(uint32_t *)lay256[i] == 256;
		for (int i = 0; i < 3; i++) ok &= *(uint32_t *)lay512[i] == 512;
		for (int i = 0; i < 2; i++) ok &= *(uint32_t *)lay255[i] == 255;
		for (int i = 0; i < 40; i++) ok &= ((uint32_t *)0x00B42450)[i] == (uint32_t)(i * 4 + 4);
		CHECK(ok, "açılışta T ve C kapalı: yerleşim 1x (ilk karakter çizilmeden önce)");
		set_pack("T", 1);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		ok = 1;
		for (int i = 0; i < 3; i++) ok &= *(uint32_t *)lay256[i] == 512;
		for (int i = 0; i < 3; i++) ok &= *(uint32_t *)lay512[i] == 1024;
		for (int i = 0; i < 40; i++) ok &= ((uint32_t *)0x00B42450)[i] == (uint32_t)(i * 4 + 4) << 1;
		CHECK(ok, "T açılınca yerleşim 2x");
		set_pack("C", 1);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		ok = 1;
		for (int i = 0; i < 3; i++) ok &= *(uint32_t *)lay256[i] == 1024;
		for (int i = 0; i < 3; i++) ok &= *(uint32_t *)lay512[i] == 2048;
		for (int i = 0; i < 2; i++) ok &= *(uint32_t *)lay255[i] == 1023;
		for (int i = 0; i < 40; i++) ok &= ((uint32_t *)0x00B42450)[i] == (uint32_t)(i * 4 + 4) << 2;
		CHECK(ok, "C (4x) de açılınca yerleşim 4x");
		set_pack("T", 0);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		CHECK(*(uint32_t *)lay256[1] == 1024, "T kapanınca 4x kalır (C hâlâ açık)");
		set_pack("C", 0);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		ok = *(uint32_t *)lay256[1] == 256 && ((uint32_t *)0x00B42450)[5] == 24;
		for (int i = 0; i < 4; i++) ok &= *(uint32_t *)alloc256[i] == 1024;
		CHECK(ok, "ikisi de kapanınca 1x, tampon 4x kaldı");
		CHECK(*(uint32_t *)pool256[0] == 256 && *(uint32_t *)pool256[1] == 256,
		      "paket aç/kapat doku havuzu sınırını değiştirmez (256)");
		/* VanillaHelpers yok ya da 1x: tampon yine paketlerin en büyüğüne göre */
		for (int i = 0; i < 4; i++) *(uint32_t *)alloc256[i] = 256;
		for (int i = 0; i < 2; i++) *(uint32_t *)pool256[i] = 256;
		((void (*)(void))0x00402B07)();
		ok = 1;
		for (int i = 0; i < 4; i++) ok &= *(uint32_t *)alloc256[i] == 1024;
		CHECK(ok, "işaret dosyası 1x iken de tampon 4x ayrıldı");
		CHECK(*(uint32_t *)pool256[0] == 256 && *(uint32_t *)pool256[1] == 256,
		      "tampon 4x büyütülürken doku havuzu sınırı yükseltilmez (256)");

		printf("== ten parçalarını bölgeye uydurma\n");
		typedef void (__fastcall *Paste_t)(void *, void *, uint8_t **, int, int32_t *, int32_t *, int32_t *, uint32_t *);
		/* çağrı noktasının (call hedef) gösterdiği fonksiyon: kurulan kanca */
		Paste_t viaSite = (Paste_t)(uintptr_t)(0x004770D7 + 5 + *(int32_t *)0x004770D8);
		Paste_t viaSite2 = (Paste_t)(uintptr_t)(0x0047715B + 5 + *(int32_t *)0x0047715C), game = (Paste_t)0x00477460;
		CHECK(*(uint8_t *)0x004770D7 == 0xE8 && 0x004770D7 + 5 + *(int32_t *)0x004770D8 != 0x00477460 &&
		      0x0047715B + 5 + *(int32_t *)0x0047715C != 0x00477460, "2 çağrı noktasına da kanca kuruldu");
		set_pack("T", 1);
		lua_reset(&L); call_lua("HDPack_Refresh", &L); /* yerleşim 2x, tampon 4x */
		int32_t origin[2] = {0, 0}, dst[2], size[2];
		pb_alloc(2);
		{ /* aynı ölçek */
			uint8_t **m = make_part(256, 128, 9, 0);
			uint32_t info[6] = {256, 128, 2, 9, 1, 0};
			dst[0] = 0, dst[1] = 0, size[0] = 256, size[1] = 128;
			viaSite(NULL, NULL, m, 0, dst, origin, size, info);
			CHECK(g_pbDrawn == 1 && g_pbOob == 0 && g_pbLevel[0][5 * 512 + 7] == part_px(0, 7, 5) &&
			      g_pbLevel[3][1 * 64 + 2] == part_px(3, 2, 1) && info[0] == 256 && info[3] == 9,
			      "2x parça 2x bölgeye olduğu gibi (oyunun bilgisi değiştirilmedi)");
		}
		{ /* 4x parça, 2x yerleşim: ilk seviye atlanır */
			uint8_t **m = make_part(512, 256, 10, 0);
			uint32_t info[6] = {512, 256, 2, 10, 1, 0};
			dst[0] = 256, dst[1] = 128, size[0] = 256, size[1] = 128;
			pb_alloc(2);
			viaSite(NULL, NULL, m, 0, dst, origin, size, info);
			int good = g_pbDrawn == 1 && g_pbOob == 0;
			for (uint32_t y = 0; y < 128; y += 9)
				for (uint32_t x = 0; x < 256; x += 7)
					good &= g_pbLevel[0][(128 + y) * 512 + 256 + x] == part_px(1, x, y);
			good &= g_pbLevel[1][(64 + 3) * 256 + 128 + 4] == part_px(2, 4, 3);
			good &= g_pbLevel[0][127 * 512 + 255] == 0xEEEE && g_pbLevel[0][128 * 512 + 512] == 0xEEEE;
			CHECK(good, "4x parça 2x bölgeye: bir seviye küçüğüyle, bölgenin dışına taşmadan");
		}
		{ /* hatanın kendisi: 2x tampon, 4x doku */
			uint8_t **m = make_part(1024, 512, 11, 0);
			uint32_t info[6] = {1024, 512, 2, 11, 1, 0};
			dst[0] = 0, dst[1] = 0, size[0] = 512, size[1] = 256;
			pb_alloc(1);
			game(NULL, NULL, m, 0, dst, origin, size, info);
			int crashed = g_pbOob > 0;
			pb_alloc(1);
			viaSite2(NULL, NULL, m, 0, dst, origin, size, info);
			CHECK(crashed && g_pbOob == 0 && g_pbDrawn == 1 && g_pbLevel[0][9 * 512 + 300] == part_px(1, 300, 9),
			      "kancasız 2x tamponun son seviyesinden sonrasına yazıyor (0x477D15 çökmesi); kancayla yazmıyor");
		}
		{ /* 1x parça, 2x yerleşim, alfa maskesiyle: büyütülür */
			uint8_t **m = make_part(128, 64, 8, 1);
			uint32_t info[6] = {128, 64, 2, 8, 1, 1};
			dst[0] = 0, dst[1] = 256, size[0] = 256, size[1] = 128;
			pb_alloc(2);
			game(NULL, NULL, m, 1, dst, origin, size, info);
			int invisible = g_pbDrawn == 0;
			pb_alloc(2);
			viaSite(NULL, NULL, m, 1, dst, origin, size, info);
			int good = g_pbDrawn == 1 && g_pbOob == 0;
			for (uint32_t y = 0; y < 128; y++)
				for (uint32_t x = 0; x < 256; x++) {
					uint16_t want = ((x / 2 + y / 2) & 1) ? part_px(0, x / 2, y / 2) : 0xEEEE;
					good &= g_pbLevel[0][(256 + y) * 512 + x] == want;
				}
			for (uint32_t y = 0; y < 64; y++)
				for (uint32_t x = 0; x < 128; x++)
					good &= g_pbLevel[1][(128 + y) * 256 + x] == (((x + y) & 1) ? part_px(0, x, y) : 0xEEEE);
			good &= g_pbLevel[2][(64 + 1) * 128 + 2] == part_px(1, 2, 1);
			CHECK(invisible, "kancasız: 1x parça 2x bölgede hiç çizilmiyor (VanillaHelpers denetimi)");
			CHECK(good, "kancayla: 1x parça ve alfa maskesi 2x'e büyütüldü, alt seviyeler dokunun kendisi");
		}
		{ /* temel ten (kaynak konumu = hedef konumu): bütün tuval */
			uint8_t **m = make_part(256, 256, 9, 0);
			uint32_t info[6] = {256, 256, 2, 9, 1, 0};
			int32_t pos[2] = {0, 0}, full[2] = {512, 512};
			pb_alloc(2);
			viaSite(NULL, NULL, m, 0, pos, pos, full, info);
			CHECK(g_pbDrawn == 1 && g_pbOob == 0 && g_pbLevel[0][301 * 512 + 411] == part_px(0, 205, 150) &&
			      g_pbLevel[1][3 * 256 + 9] == part_px(0, 9, 3), "1x temel ten 2x tuvale büyütüldü");
		}
		{ /* oranı tutmayan parçalar */
			uint8_t **m = make_part(128, 128, 8, 0);
			uint32_t info[6] = {128, 128, 2, 8, 1, 0};
			dst[0] = 0, dst[1] = 0, size[0] = 256, size[1] = 128;
			pb_alloc(2);
			viaSite(NULL, NULL, m, 0, dst, origin, size, info);
			read_file(log, buf, sizeof buf);
			CHECK(g_pbCalls == 0 && strstr(buf, "skin part 128x128 does not fit region 256x128: skipped"),
			      "bölgeden küçük ve oranı farklı parça atlandı, günlüğe yazıldı");
			uint8_t **b = make_part(512, 128, 10, 0);
			uint32_t binfo[6] = {512, 128, 2, 12, 1, 0};
			pb_alloc(1);
			viaSite(NULL, NULL, b, 0, dst, origin, size, binfo);
			CHECK(g_pbDrawn == 1 && g_pbOob == 0 && g_pbLevel[0][2 * 512 + 3] == part_px(0, 3, 2),
			      "büyük ve oranı farklı parça oyunun yaptığı gibi kırpıldı, seviye sayısı tamponu aşmadı");
		}
		{ /* aynı ölçekte bile fazla seviye bildiren doku tamponu aşmaz */
			uint8_t **m = make_part(512, 256, 12, 0);
			uint32_t info[6] = {512, 256, 2, 12, 1, 0};
			dst[0] = 0, dst[1] = 0, size[0] = 512, size[1] = 256;
			pb_alloc(1);
			viaSite(NULL, NULL, m, 0, dst, origin, size, info);
			CHECK(g_pbDrawn == 1 && g_pbOob == 0, "seviye sayısı yerleşimin seviye sayısıyla sınırlandı");
		}
		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "skin part 512x256 fitted to region 256x128") && strstr(buf, "skin part 128x64 fitted to region 256x128"),
		      "uydurulan parçalar günlüğe yazıldı");
		for (int i = 0; i < 3; i++) *(uint32_t *)lay256[i] = 0; /* sonraki bölümler için etkisiz */
		set_pack("T", 1); set_pack("C", 0);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
	}

	printf("== binalar ve dünyayı yeniden yükleme\n");
	/* 4 kova; kova 1: Wolf(.wmo yok) -> NewWolf ; bina adları sahte arşiv dosyalarına eşlenir */
	memset(g_moBuckets, 0, sizeof g_moBuckets);
	const char *moNames[3] = {"Creature\\Wolf.m2", "Creature\\NewWolf.m2", "Item\\Sword.blp"};
	for (int i = 0; i < 3; i++) {
		g_mo[i] = calloc(1, 0x200);
		strcpy((char *)g_mo[i] + 0x1C, moNames[i]);
		*(uint32_t *)(g_mo[i] + 4) = 0x100 + 1; /* kova 1 */
	}
	*(uint32_t *)(g_moBuckets + 1 * 12) = 0x40;                 /* bağlantı ofseti */
	*(uint8_t **)(g_moBuckets + 1 * 12 + 8) = g_mo[0];
	*(uint8_t **)(g_mo[0] + 0x40 + 4) = g_mo[1];
	*(uint8_t **)(g_mo[1] + 0x40 + 4) = g_mo[2];
	*(uintptr_t *)(g_mo[2] + 0x40 + 4) = 1;
	*(uint8_t **)0x00CA8054 = g_moBuckets;
	*(uint32_t *)0x00CA805C = 3;
	*(uintptr_t *)0x00C96088 = 0x7000; /* dünya yüklü (bir alan var) */
	*(uint32_t *)0x00C62954 = 0;
	g_unloadAll = 0; g_doodadCreated = 0;
	set_pack("C", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_unloadAll == 1 && *(uint32_t *)0x00C62954 == 1, "dünya oyunun ışınlanma yoluyla boşaltıldı, ışık anında güncellenecek");
	CHECK((*(uint32_t *)(g_mo[0] + 4) & 3) != 1 && (*(uint32_t *)(g_mo[0] + 4) & ~3u) == 0x100,
	      "dosyası değişen bina (C -> patch-5) hash'i kovaya düşemeyecek şekilde değişti");
	CHECK(*(uint32_t *)(g_mo[1] + 4) == 0x101 && *(uint32_t *)(g_mo[2] + 4) == 0x101,
	      "yalnız-C ve ilgisiz binaların hash'i değişmedi");
	CHECK(g_doodadCreated == 0, "dünya yeniden yüklenirken dekorlar ayrıca kurulmadı");
	*(uintptr_t *)0x00C96088 = 1; /* dünya yüklü değil (boş liste) */
	g_unloadAll = 0;
	set_pack("C", 0);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_unloadAll == 0, "dünya yüklü değilken boşaltma çağrılmaz");
	*(uintptr_t *)0x00C96088 = 0x7000;
	g_unloadAll = 0;
	CHECK(!set_pack("S", 0) || 1, "S paketi (sahte düzenekte yüklü değil) ayarlanabilir");
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	CHECK(g_unloadAll == 0, "yalnız ses paketi değişince dünya yeniden yüklenmez");
	set_pack("S", 1);
	lua_reset(&L); call_lua("HDPack_Refresh", &L);
	printf("== karakter seçim / oluşturma ekranları (glue)\n");
	{
		((void (*)(void))0x0046A880)();
		CHECK(g_glueRegisterCalls == 1, "oyunun glue fonksiyon kaydı çağrıldı (zincir korunur)");
		CHECK(lua_fn("HDPack_RefreshGlue") != NULL, "glue'da HDPack_RefreshGlue kaydedildi");
		CHECK(g_execCount == 1 && !strcmp(g_execName, "HDSwitch") &&
		          !strncmp(g_execCode, "-- HD Switch on the character select", 36),
		      "glue Lua kodu (dll/glue.lua) çalıştırıldı");
		for (int i = 0; i < 0x16C; i++)
			g_comp[0x18 + i] = (uint8_t)(i * 7 + 1);
		*(void **)(g_comp + 0x38) = (void *)0x1234; /* model */
		*(uint8_t **)0x00B42078 = g_comp;
		*(void **)0x00B420F0 = (void *)0x5678;
		build_objects();
		g_rebuilt = 0; g_unloadAll = 0; g_createApplies = 0;
		*(uintptr_t *)0x00C96088 = 0x7000; /* dünya yüklü görünse bile */
		set_pack("C", 1);
		lua_reset(&L); push_s(&L, "create");
		call_lua("HDPack_RefreshGlue", &L);
		CHECK(served("Creature\\Wolf.m2") == 2, "glue yenilemesi seçimi uyguladı");
		CHECK(g_unloadAll == 0 && g_rebuilt == 0, "glue'da dünya boşaltma ve karakter/nesne yeniden kurma çalışmaz");
		CHECK(g_createApplies == 1 && g_createFlag == 1 && g_createDescOk,
		      "oluşturulan karakter mevcut tanımın kopyasıyla yeniden kuruldu (ırk/cinsiyet/ten/yüz/saç korunur)");
		lua_reset(&L); push_s(&L, "create");
		call_lua("HDPack_RefreshGlue", &L);
		CHECK(g_createApplies == 1, "değişiklik yoksa karakter yeniden kurulmaz");
		/* karakter seçim: her karakterin saklanan bileşeni/modeli (girdi +0x110) */
		memset(g_charList, 0, sizeof g_charList);
		*(void **)(g_charList + 0 * 0x120 + 0x110) = (void *)0x1111;
		*(void **)(g_charList + 0 * 0x120 + 0x114) = (void *)0x2222;
		*(void **)(g_charList + 2 * 0x120 + 0x110) = (void *)0x3333;
		*(uint8_t **)0x00B42144 = g_charList;
		*(uint32_t *)0x00B42140 = 3;
		g_entryReleases = 0;
		set_pack("C", 0);
		lua_reset(&L); push_s(&L, "select");
		call_lua("HDPack_RefreshGlue", &L);
		CHECK(g_createApplies == 1 && served("Creature\\Wolf.m2") == 3, "karakter seçimde oluşturma karakterine dokunulmaz");
		CHECK(g_entryReleases == 2 && !*(void **)(g_charList + 0x110) && !*(void **)(g_charList + 2 * 0x120 + 0x110),
		      "karakter seçim: saklanan karakterler oyunun kendi yoluyla bırakıldı (seçilince yeni dosyalarla kurulur)");
		CHECK(!*(void **)(g_charList + 0x114), "bırakılan modelin alanı temizlendi (iki kez bırakılmaz)");
		lua_reset(&L); push_s(&L, "select");
		call_lua("HDPack_RefreshGlue", &L);
		CHECK(g_entryReleases == 2, "değişiklik yoksa karakterler bırakılmaz");
		*(void **)0x00B42144 = NULL;
		*(uint32_t *)0x00B42140 = 0;
		*(void **)(g_comp + 0x38) = NULL;
		set_pack("C", 1);
		lua_reset(&L); push_s(&L, "create");
		call_lua("HDPack_RefreshGlue", &L);
		CHECK(g_createApplies == 1, "modeli olmayan bileşende yeniden kurma yapılmaz");
		*(uint8_t **)0x00B42078 = NULL;
		*(void **)0x00B420F0 = NULL;
		set_pack("C", 0);
		lua_reset(&L); push_s(&L, "none");
		call_lua("HDPack_RefreshGlue", &L);
		*(uint8_t **)0x00B41414 = NULL;
	}
	*(uintptr_t *)0x00C96088 = 1;
	*(uint8_t **)0x00CA8054 = NULL;
	set_pack("C", 0); set_pack("T", 0); /* sonraki bölümün beklediği durum */

	printf("== teşhis: yüklenemeyen dokular\n");
	{
		const char *nm = "Interface\\Buttons\\UI-Panel-Button-Up.blp";
		uint32_t espBefore, espAfter;
		__asm__ volatile("movl %%esp, %0" : "=r"(espBefore));
		__asm__ volatile("pushl %%esi\n\t"
		                 "movl %0, %%esi\n\t"
		                 "movl $0x4488F8, %%eax\n\t"
		                 "call *%%eax\n\t"
		                 "popl %%esi"
		                 :: "r"(nm) : "eax", "ecx", "edx", "memory");
		__asm__ volatile("movl %%esp, %0" : "=r"(espAfter));
		CHECK(g_lockDefaults == 1 && espBefore == espAfter, "yeniden okuma hatası: asıl fonksiyon çağrıldı, yığın dengede");
		uint8_t *t = calloc(1, 0x200);
		strcpy((char *)t + 0x0C, "Interface\\Buttons\\UI-SliderBar-Button-Horizontal.blp");
		((void (__fastcall *)(void *, void *))0x0044A513)((void *)0x90582C, t);
		CHECK(g_asyncDefaults == 1, "ilk yükleme hatası: asıl fonksiyon çağrıldı");
		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "texture failed to load (re-read): Interface\\Buttons\\UI-Panel-Button-Up.blp") &&
		          strstr(buf, "texture failed to load (first load): Interface\\Buttons\\UI-SliderBar-Button-Horizontal.blp"),
		      "iki hata da doku adıyla günlüğe yazıldı");
		((void (__fastcall *)(void *, void *))0x0044A513)((void *)0x90582C, t);
		read_file(log, buf, sizeof buf);
		char *first = strstr(buf, "UI-SliderBar"), *second = first ? strstr(first + 1, "UI-SliderBar") : NULL;
		CHECK(g_asyncDefaults == 2 && !second, "aynı doku ikinci kez yazılmaz");
	}

	printf("== ten doku önbelleğinde eskimiş boyut bilgisi\n");
	{
		static const uintptr_t sites[] = {0x00476EF2u, 0x00476FA4u, 0x00477025u, 0x004770B2u, 0x0047712Du};
		int hooked = 0;
		for (int i = 0; i < 5; i++)
			hooked += *(uint8_t *)sites[i] == 0xE8 && sites[i] + 5 + *(int32_t *)(sites[i] + 1) != 0x0047B150;
		CHECK(hooked == 5, "5 çağrı noktasının hepsine kanca kuruldu");
		typedef int (__fastcall *Info_t)(uint8_t *entry, void *out, int flag);
		Info_t info = (Info_t)(uintptr_t)(0x0047712D + 5 + *(int32_t *)0x0047712E);
		static uint8_t tex[0x200], entry[0xD0], bits[16];
		strcpy((char *)tex + 0x0C, "Character\\Troll\\Female\\TrollFemaleFaceLower00_00.blp");
		*(void **)(tex + 0x120) = bits;
		*(uint32_t *)(tex + 0x144) = 256; *(uint32_t *)(tex + 0x148) = 256; *(uint32_t *)(tex + 0x118) = 2;
		*(uint8_t **)(entry + 0x18) = tex;
		/* önbellek HD sürümünün bilgisini tutuyor (512x512) */
		*(uint32_t *)(entry + 0x1C) = 512; *(uint32_t *)(entry + 0x20) = 512; *(uint32_t *)(entry + 0x24) = 2;
		g_texInfoCalls = 0;
		int r = info(entry, NULL, 1);
		CHECK(r == 1 && g_texInfoCalls == 1 && g_texInfoSeenW == 0,
		      "doku 256x256'ya geçmişken önbellekteki 512x512 bilgisi oyuna gitmeden silindi (yeniden hesaplanır)");
		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "texture cache: outdated size 512x512 -> 256x256") != NULL, "günlüğe yazıldı");
		*(uint32_t *)(entry + 0x1C) = 256; *(uint32_t *)(entry + 0x20) = 256;
		info(entry, NULL, 1);
		CHECK(g_texInfoSeenW == 256, "güncel bilgiye dokunulmadı");
		*(uint32_t *)(entry + 0x1C) = 512;
		*(void **)(tex + 0x120) = NULL;
		info(entry, NULL, 1);
		CHECK(g_texInfoSeenW == 512, "henüz yüklenmemiş dokuda (görüntü yok) bilgiye dokunulmadı");
		*(uint8_t **)(entry + 0x18) = NULL;
		info(entry, NULL, 1);
		CHECK(g_texInfoCalls == 4, "dokusu olmayan girdi de aynen oyuna iletildi");
	}
	printf("== eksik mip zinciri (beyaz dokular)\n");
	{
		typedef int (__fastcall *Site_t)(uint8_t *blp, void *e, const char *name, int flag);
		/* oyundaki gibi: argümanlar yığında, çağrı noktasının hedefi doğrudan çağrılır */
		Site_t site = (Site_t)(uintptr_t)(0x004489F6 + 5 + *(int32_t *)0x004489F7);
		static uint8_t blp[0x4C0];
		uint32_t *ofs = (uint32_t *)(blp + 0x18), *size = (uint32_t *)(blp + 0x58);
		CHECK(*(uint8_t *)0x004489F6 == 0xE8 && 0x004489F6 + 5 + *(int32_t *)0x004489F7 != 0x005A7B40,
		      "yeniden yükleme yolundaki başlık okuma çağrısına kanca kuruldu");
		g_blpOpens = 0;
		int r = site(blp, NULL, "Border", 2);
		CHECK(r == 1 && g_blpOpens == 1, "asıl başlık okuma çağrıldı, sonucu aynen döndü");
		CHECK(size[9] == 16 && ofs[9] == ofs[8] && size[8] == 16 && ofs[8] == 88580,
		      "büyü çubuğu kenarı (DXT5 512x128): eksik 1x1 seviyesi 2x1'in tek bloğuyla tamamlandı");
		CHECK(size[0] == 65536 && size[7] == 16, "var olan seviyelere dokunulmadı");
		site(blp, NULL, "Dagger", 0);
		CHECK(size[9] == 8 && ofs[9] == 88556, "hançer (DXT1 512x256): kaydı hiç olmayan 1x1 seviyesi tamamlandı");
		site(blp, NULL, "BigHole", 0);
		CHECK(size[2] == 0, "tek blok olmayan eksik seviye uydurulmadı");
		site(blp, NULL, "Palette", 0);
		CHECK(size[1] == 0, "DXT olmayan dosyaya dokunulmadı");
		CHECK(site(blp, NULL, "Yok", 0) == 0, "açılamayan dosyada sonuç 0 kaldı");
		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "mip chain completed (missing last level in the pack): Border") != NULL, "onarılan dosya günlüğe yazıldı");
	}
	printf("== Z birleştirme yaması\n");
	set_pack("C", 1); set_pack("T", 1); apply_packs();
	CHECK(served("DBFilesClient\\SoundEntries.dbc") == 0, "C açıkken Z'nin birleşik DBC'si kullanılır");
	set_pack("C", 0); apply_packs();
	CHECK(served("DBFilesClient\\SoundEntries.dbc") == 4, "C kapalıyken Z'nin birleşik DBC'si de atlanır (orijinale düşer)");
	set_pack("T", 0); apply_packs();
	CHECK(served("DBFilesClient\\SoundEntries.dbc") == 4, "ilgisiz paket kapalıyken de doğru");
	set_pack("C", 1); apply_packs();
	CHECK(served("DBFilesClient\\SoundEntries.dbc") == 0, "yalnız T kapalıyken (DBC'yi içermeyen) Z kullanılır");
	set_pack("C", 0); set_pack("T", 0); apply_packs(); /* sonraki bölümün beklediği durum */

	printf("== H düzeltme paketi\n");
	set_pack("C", 1); set_pack("T", 1); apply_packs();
	CHECK(served("Creature\\Bat.m2") == 6, "hepsi açık: düzeltilmiş kopya (H) kullanılır");
	CHECK(served("Creature\\Glow.blp") == 6, "yalnız H'de olan doku H'den okunur");
	set_pack("C", 0); apply_packs();
	CHECK(served("Creature\\Bat.m2") == 5, "C kapalı: C'nin dosyasının düzeltilmiş kopyası da atlanır (orijinale düşer)");
	CHECK(served("Creature\\Wolf.m2") == 3, "C kapalı: H'de olmayan dosyalar eskisi gibi");
	CHECK(served("Creature\\Glow.blp") == 6, "C kapalı: yalnız H'de olan doku etkilenmez");
	set_pack("C", 1); set_pack("H", 0); apply_packs();
	CHECK(served("Creature\\Bat.m2") == 2, "H kapalı: C'nin düzeltilmemiş dosyası");
	set_pack("H", 1); set_pack("T", 0); apply_packs();
	CHECK(served("Creature\\Bat.m2") == 6, "dosyayı içermeyen paket (T) kapalıyken H kullanılır");
	set_pack("C", 0); set_pack("T", 0); apply_packs(); /* sonraki bölümün beklediği durum */

	printf("== V düzeltme paketi (U için)\n");
	set_pack("C", 1); set_pack("T", 1); apply_packs();
	CHECK(served("Character\\Face.blp") == 7, "hepsi açık: düzeltilmiş kopya (V) kullanılır");
	set_pack("T", 0); apply_packs();
	CHECK(served("Character\\Face.blp") == 4, "T kapalı: V'nin kopyası da atlanır (orijinale düşer)");
	CHECK(served("Character\\VOnly.blp") == 7, "T kapalı: yalnız V'de olan doku etkilenmez");
	set_pack("T", 1); set_pack("V", 0); apply_packs();
	CHECK(served("Character\\Face.blp") == 1, "V kapalı: T'nin düzeltilmemiş dosyası");
	set_pack("V", 1); set_pack("C", 0); set_pack("T", 0); apply_packs(); /* sonraki bölümün beklediği durum */

	printf("== kalıcılık\n");
	GetPrivateProfileStringA("HDToggle", "off", "?", buf, sizeof buf, ini);
	CHECK(!strcmp(buf, "CT"), "ayar dosyası: off=CT");
	set_pack("T", 1);
	set_pack("C", 1);
	GetPrivateProfileStringA("HDToggle", "off", "?", buf, sizeof buf, ini);
	CHECK(buf[0] == 0, "hepsi açılınca off boş");
	apply_packs();
	CHECK(served("Creature\\Wolf.m2") == 2 && served("Creature\\Skin.blp") == 1, "hepsi açıkken doğal sıra");

	printf("== yük\n");
	set_pack("C", 0); apply_packs();
	int before = g_searchCalls;
	DWORD t0 = GetTickCount();
	for (int i = 0; i < 100000; i++)
		served("Item\\Sword.blp");
	DWORD dt = GetTickCount() - t0;
	printf("  100000 arama, %lu ms, arama başına %.1f iç çağrı\n", dt, (g_searchCalls - before) / 100000.0);
	CHECK(dt < 3000, "kapalı paket varken arama hızlı");

	printf("== öz-sınama (gerçek oyundaki [Test] modu)\n");
	FreeLibrary(h);
	/* DLL kancaları kaldırmaz; düzeneği baştan kurup DLL'i yeniden yükle */
	build_world();
	DeleteFileA(ini);
	WritePrivateProfileStringA("HDToggle", "off", "T", ini); /* kullanıcı ayarı: öz-sınama etkilememeli */
	WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini); /* testte gerçek ağa çıkma */
	WritePrivateProfileStringA("Test", "selftest", "1", ini);
	WritePrivateProfileStringA("Test", "t1", "C|Creature\\Wolf.m2|2016|3016", ini);      /* C kapalı: patch-5 */
	WritePrivateProfileStringA("Test", "t2", "C|Creature\\NewWolf.m2|2019|2019", ini);   /* yalnız C'de */
	WritePrivateProfileStringA("Test", "t3", "T|Creature\\Skin.blp|1017|2017", ini);    /* T kapalı: C */
	WritePrivateProfileStringA("Test", "t4", "T|Item\\Sword.blp|1015|9999", ini);        /* bilerek yanlış */
	h = LoadLibraryA(dll);
	for (int w = 0; w < 200; w++) {
		read_file(log, buf, sizeof buf);
		if (strstr(buf, "selftest done"))
			break;
		Sleep(100);
	}
	CHECK(strstr(buf, "PASS C Creature\\Wolf.m2") != NULL, "öz-sınama: kapalı pakette alttaki sürüm ölçülür");
	CHECK(strstr(buf, "PASS C Creature\\NewWolf.m2") != NULL, "öz-sınama: paket-özel dosya");
	CHECK(strstr(buf, "PASS T Creature\\Skin.blp") != NULL, "öz-sınama: T kapalıyken C");
	CHECK(strstr(buf, "FAIL T Item\\Sword.blp") != NULL, "öz-sınama: yanlış beklenti KALDI olarak raporlanır");
	CHECK(strstr(buf, "3 passed, 1 failed") != NULL, "öz-sınama özeti");
	CHECK(served("Creature\\Skin.blp") == 2, "öz-sınama kullanıcının ayarını (T kapalı) bozmaz");


	printf("== sunucu denetimi (Capycraft ve Ravencraft dışında her yerde)\n");
	{
		static const struct { const char *realmlist, *realm; int ok; const char *why; } cases[] = {
			{"play.octowow.st", "N'Zoth", 1, "OctoWoW giriş sunucusu"},
			{"PLAY.OctoWoW.ST:3724", NULL, 1, "büyük harf ve port ekiyle"},
			{"normal.octowow.st", "Y'Shaarj", 1, "başka OctoWoW adı ve realm'ı"},
			{"logon.ravencraft.io", "Medivh", 0, "Ravencraft"},
			{"apac.capycraft.io", NULL, 0, "Capycraft"},
			{"APAC.CapyCraft.IO:3724", NULL, 0, "Capycraft, büyük harf ve port ekiyle"},
			{"ravencraft.io", NULL, 0, "Ravencraft, yalın alan adı"},
			{"logon.ravencraft.io", "N'Zoth", 0, "Ravencraft adresi (realm adı OctoWoW'unki olsa da)"},
			{"logon.ravencraft.com", NULL, 1, "benzer ama farklı uzantı"},
			{"ravencraft.io.example.com", NULL, 1, "sonu listelenmiş alan adı olmayan adres"},
			{"notcapycraft.io", NULL, 1, "benzer ama farklı alan adı"},
			{"logon.kronos-wow.com", "Nordanaar", 1, "listede olmayan başka bir sunucu"},
			{"127.0.0.1", "C'Thun", 1, "yerel proxy + OctoWoW realm'ı"},
			{"127.0.0.1", NULL, 1, "yerel proxy, realm henüz seçilmedi"},
			{"127.0.0.1", "Medivh", 0, "yerel proxy + Ravencraft realm'ı"},
			{"localhost", "\xE6\xB0\xB8\xE6\xAD\x8C\xE8\x8D\x92\xE9\x87\x8E", 0, "localhost + Capycraft realm'ı (永歌荒野)"},
			{"185.246.188.177", "\xE5\x9F\xBA\xE8\xB5\xAB\xE7\xBA\xB3\xE6\x96\xAF", 0, "çıplak IP + Capycraft realm'ı (基赫纳斯)"},
			{"185.246.188.177", "\xE7\xBE\xA4\xE6\x98\x9F\xE7\x9B\x86\xE5\x9C\xB0", 0, "çıplak IP + Capycraft realm'ı (群星盆地)"},
			{"185.246.188.177", "MEDIVH", 0, "çıplak IP + Ravencraft realm'ı, büyük harf"},
			{"92.114.107.48:3724", "Nordanaar", 1, "çıplak IP + listede olmayan realm"},
			{"185.246.188.177", NULL, 1, "çıplak IP, realm henüz seçilmedi"},
			{"play.octowow.st", "Medivh", 1, "OctoWoW adresi: realm adından bağımsız çalışır"},
			{NULL, NULL, 1, "realmlist yok"},
		};
		for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
			FreeLibrary(h);
			build_world();
			DeleteFileA(ini);
			WritePrivateProfileStringA("HDToggle", "off", "C", ini);
			WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini); /* testte gerçek ağa çıkma */
			write_server(cases[c].realmlist, cases[c].realm);
			h = LoadLibraryA(dll);
			g_nreg = 0;
			((void (*)(void))0x0048FE9C)();
			FakeL L;
			lua_reset(&L);
			int n = call_lua("HDPack_Info", &L);
			int locked = n == 4 && L.v[3].n == 1;
			char name[160];
			snprintf(name, sizeof name, "%s: %s", cases[c].why, cases[c].ok ? "çalışır" : "kilitli");
			CHECK(locked == !cases[c].ok, name);
			if (!cases[c].ok) {
				CHECK(served("Creature\\Wolf.m2") == 2, "  kilitliyken kayıtlı seçim (C kapalı) uygulanmaz, paket kurulu haliyle");
				lua_reset(&L);
				CHECK(call_lua("HDPack_List", &L) == 0, "  kilitliyken paket listesi boş");
				CHECK(!set_pack("C", 0) && served("Creature\\Wolf.m2") == 2, "  kilitliyken paket kapatılamaz");
				if (c == 3) {
					static const uintptr_t alloc256[] = {0x4760A8, 0x4760AD, 0x476792, 0x476797};
					for (int i = 0; i < 4; i++) *(uint32_t *)alloc256[i] = 512;
					*(uint32_t *)0x44AFBA = *(uint32_t *)0x44AFC6 = 512;
					*(uint32_t *)0x4778D3 = *(uint32_t *)0x477930 = *(uint32_t *)0x477935 = 512;
					*(uint32_t *)0x477CA1 = *(uint32_t *)0x477DD1 = *(uint32_t *)0x477F43 = 1024;
					((void (*)(void))0x00402B07)();
					int same = *(uint32_t *)0x477930 == 512;
					for (int i = 0; i < 4; i++) same &= *(uint32_t *)alloc256[i] == 512;
					CHECK(same, "  kilitliyken ten ölçeği VanillaHelpers'ın seçimi (tampon büyütülmez)");
					CHECK(*(uint32_t *)0x44AFBA == 256 && *(uint32_t *)0x44AFC6 == 256,
					      "  kilitliyken de doku havuzu sınırı oyunun kendi değerine (256) döner");
					/* beklenmeyen kod (başka exe sürümü / başka mod): dokunulmaz */
					*(uint8_t *)0x44AFB9 = 0x90; *(uint32_t *)0x44AFBA = 512; *(uint32_t *)0x44AFC6 = 512;
					((void (*)(void))0x00402B07)();
					CHECK(*(uint32_t *)0x44AFBA == 512 && *(uint32_t *)0x44AFC6 == 256,
					      "  komut beklenen cmp değilse o sabite dokunulmaz, diğeri yine düzelir");
					*(uint8_t *)0x44AFB9 = 0xFA;
					typedef void (__fastcall *Paste_t)(void *, void *, uint8_t **, int, int32_t *, int32_t *, int32_t *, uint32_t *);
					uint8_t **m = make_part(128, 64, 8, 0);
					uint32_t info[6] = {128, 64, 2, 8, 1, 0};
					int32_t o[2] = {0, 0}, d[2] = {0, 0}, sz[2] = {256, 128};
					pb_alloc(1);
					((Paste_t)(uintptr_t)(0x004770D7 + 5 + *(int32_t *)0x004770D8))(NULL, NULL, m, 0, d, o, sz, info);
					CHECK(g_pbCalls == 1 && g_pbDrawn == 0, "  kilitliyken parçalar uydurulmaz (oyunun kendi davranışı)");
				}
			} else {
				CHECK(served("Creature\\Wolf.m2") == 3, "  açılışta kayıtlı seçim (C kapalı) uygulanır");
			}
		}

		/* proxy arkasında OctoWoW realm'ıyla açıldı, oyunda başka realm seçildi */
		FreeLibrary(h);
		build_world();
		DeleteFileA(ini);
		WritePrivateProfileStringA("HDToggle", "off", "C", ini);
		WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini); /* testte gerçek ağa çıkma */
		write_server("127.0.0.1", "N'Zoth");
		h = LoadLibraryA(dll);
		g_nreg = 0;
		((void (*)(void))0x0048FE9C)();
		set_cvar(0x00C2812C, g_cvarList, "127.0.0.1");
		set_cvar(0x00C28130, g_cvarName, "N'Zoth");
		FakeL L;
		lua_reset(&L);
		call_lua("HDPack_Info", &L);
		CHECK(L.v[3].n == 0 && served("Creature\\Wolf.m2") == 3, "oyunda OctoWoW realm'ı: çalışır, C kapalı");
		set_cvar(0x00C28130, g_cvarName, "Medivh");
		lua_reset(&L);
		call_lua("HDPack_Info", &L);
		CHECK(L.v[1].n == 0 && L.v[3].n == 1 && !strcmp(L.v[2].s, "not available on this server"),
		      "Ravencraft realm'ı seçilince kilitlenir");
		CHECK(served("Creature\\Wolf.m2") == 3, "geri açma hemen değil (dünya yüklenirken olabilir), eklentinin yenilemesini bekler");
		lua_reset(&L);
		CHECK(call_lua("HDPack_Refresh", &L) == 1 && L.v[0].type == 0, "kilitliyken Refresh nil döner");
		CHECK(served("Creature\\Wolf.m2") == 2, "o yenileme bütün paketleri geri açtı");
		GetPrivateProfileStringA("HDToggle", "off", "?", buf, sizeof buf, ini);
		CHECK(!strcmp(buf, "C"), "kullanıcının OctoWoW için kayıtlı seçimi silinmedi");
		set_cvar(0x00C28130, g_cvarName, "N'Zoth");
		lua_reset(&L);
		call_lua("HDPack_Info", &L);
		CHECK(L.v[3].n == 1, "kilit oturum boyunca kalır (realm geri seçilse de; yeniden başlatınca açılır)");

		/* realmlist oyunda değiştirildi (/console realmlist ...) */
		FreeLibrary(h);
		build_world();
		write_server("play.octowow.st", NULL);
		h = LoadLibraryA(dll);
		g_nreg = 0;
		((void (*)(void))0x0048FE9C)();
		set_cvar(0x00C2812C, g_cvarList, "logon.ravencraft.io");
		lua_reset(&L);
		call_lua("HDPack_Info", &L);
		CHECK(L.v[3].n == 1, "oyun içinde Ravencraft'a çevrilen realmlist kilitler (dosya değil, oyunun değeri esas)");

		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "server not supported") != NULL, "günlükte kilit nedeni yazılı");
	}
	{
		/* DLL'de düz metin alan adı / realm adı yok */
		char path[MAX_PATH];
		GetModuleFileNameA(h, path, sizeof path);
		FILE *f = fopen(path, "rb");
		char *img = malloc(1 << 20);
		size_t len = f && img ? fread(img, 1, 1 << 20, f) : 0;
		if (f)
			fclose(f);
		int found = 0;
		static const char *const words[] = {"octowow.st", "N'Zoth", "C'Thun", "Y'Shaarj", "Zoth", "Thun", "Shaarj", "capycraft", "ravencraft", "Medivh"};
		for (size_t w = 0; w < sizeof words / sizeof words[0]; w++)
			for (size_t i = 0; i + strlen(words[w]) <= len; i++)
				if (!_strnicmp(img + i, words[w], strlen(words[w])))
					found++;
		CHECK(len > 0 && found == 0, "DLL dosyasında sunucu/realm adları düz metin olarak geçmiyor");
		free(img);
	}

	printf("== güncelleme kontrolü\n");
	{
		const char *port = getenv("HDT_TEST_PORT");
		static const struct { const char *file; const char *want; const char *why; } cases[] = {
			{"newer.json", "9.9.9", "daha yeni sürüm bildirilir"},
			{"older.json", NULL, "eski sürüm bildirilmez"},
			{"bad.json", NULL, "kuralına uymayan etiket (rakam ve nokta dışı) kabul edilmez"},
			{"missing.json", NULL, "cevap yoksa bir şey bildirilmez"},
		};
		for (size_t c = 0; port && c < sizeof cases / sizeof cases[0]; c++) {
			char url[128];
			snprintf(url, sizeof url, "http://127.0.0.1:%s/%s", port, cases[c].file);
			FreeLibrary(h);
			for (int w = 0; w < 300 && GetModuleHandleA("HDToggle.dll"); w++)
				Sleep(50);
			build_world();
			DeleteFileA(ini);
			WritePrivateProfileStringA("HDToggle", "updateurl", url, ini);
			WritePrivateProfileStringA("HDToggle", "updatedelay", "0", ini);
			write_server("play.octowow.st", "N'Zoth");
			h = LoadLibraryA(dll);
			g_nreg = 0;
			((void (*)(void))0x0048FE9C)();
			for (int w = 0; w < 100; w++) {
				read_file(log, buf, sizeof buf);
				if (strstr(buf, "update check:"))
					break;
				Sleep(100);
			}
			FakeL L;
			lua_reset(&L);
			call_lua("HDPack_Update", &L);
			int ok = cases[c].want ? (L.v[0].type == 4 && !strcmp(L.v[0].s, cases[c].want)) : L.v[0].type == 0;
			CHECK(ok && strstr(buf, "update check:") != NULL, cases[c].why);
		}
		if (!port)
			printf("  (HDT_TEST_PORT yok: güncelleme testleri atlandı)\n");
		FreeLibrary(h);
		for (int w = 0; w < 300 && GetModuleHandleA("HDToggle.dll"); w++)
			Sleep(50);
		build_world();
		DeleteFileA(ini);
		WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini);
		WritePrivateProfileStringA("HDToggle", "updateurl", "http://127.0.0.1:1/x", ini);
		WritePrivateProfileStringA("HDToggle", "updatedelay", "0", ini);
		h = LoadLibraryA(dll);
		g_nreg = 0;
		((void (*)(void))0x0048FE9C)();
		Sleep(500);
		read_file(log, buf, sizeof buf);
		{
			FakeL L;
			lua_reset(&L);
			call_lua("HDPack_Update", &L);
			CHECK(!strstr(buf, "update check:") && L.v[0].type == 0, "updatecheck=0 iken hiç istek yapılmaz");
		}
		FreeLibrary(h);
		build_world();
		DeleteFileA(ini);
		WritePrivateProfileStringA("HDToggle", "updatedelay", "0", ini);
		if (port) {
			char url[128];
			snprintf(url, sizeof url, "http://127.0.0.1:%s/newer.json", port);
			WritePrivateProfileStringA("HDToggle", "updateurl", url, ini);
		}
		write_server("logon.ravencraft.io", NULL);
		h = LoadLibraryA(dll);
		Sleep(800);
		read_file(log, buf, sizeof buf);
		CHECK(!strstr(buf, "update check:"), "engelli sunucuda güncelleme kontrolü yapılmaz");
		/* boşaltılırken çalışan kontrol beklenir: gecikme 3 sn, hemen boşalt */
		FreeLibrary(h);
		build_world();
		DeleteFileA(ini);
		WritePrivateProfileStringA("HDToggle", "updatedelay", "3000", ini);
		write_server("play.octowow.st", "N'Zoth");
		if (port) {
			char url[128];
			snprintf(url, sizeof url, "http://127.0.0.1:%s/newer.json", port);
			WritePrivateProfileStringA("HDToggle", "updateurl", url, ini);
		}
		h = LoadLibraryA(dll);
		DWORD t0 = GetTickCount();
		FreeLibrary(h);
		int heldWhileRunning = GetModuleHandleA("HDToggle.dll") != NULL;
		while (GetModuleHandleA("HDToggle.dll") && GetTickCount() - t0 < 15000)
			Sleep(50);
		DWORD took = GetTickCount() - t0;
		CHECK(heldWhileRunning && !GetModuleHandleA("HDToggle.dll") && took >= 2500 && took < 12000,
		      "kontrol sürerken DLL boşaltılmadı, kontrol bitince kendisi bıraktı (çökme ve kilitlenme yok)");
		build_world();
		h = LoadLibraryA(dll);
	}
	printf("== paket denetimi (HDPack_Scan)\n");
	{
		char sd[MAX_PATH], fp[MAX_PATH + 32];
		GetFullPathNameA("scan_data", sizeof sd, sd, NULL);
		CreateDirectoryA(sd, NULL);
		static const char *junk[] = {"patch-A.mpq", "patch-b.mpq", "patch-C.mpq", "patch-5.mpq", "Patch-LL.mpq"};
		for (size_t j = 0; j < sizeof junk / sizeof junk[0]; j++)
			snprintf(fp, sizeof fp, "%s\\%s", sd, junk[j]), DeleteFileA(fp);
		HANDLE f;
		DWORD w;
		snprintf(fp, sizeof fp, "%s\\patch-A.mpq", sd);
		f = CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); WriteFile(f, "abc", 3, &w, NULL); CloseHandle(f);
		snprintf(fp, sizeof fp, "%s\\patch-b.mpq", sd);
		CloseHandle(CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		snprintf(fp, sizeof fp, "%s\\patch-C.mpq", sd);
		size_t bigN = 1572865;
		uint8_t *big = malloc(bigN);
		for (size_t j = 0; j < bigN; j++)
			big[j] = (uint8_t)(j % 251);
		f = CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); WriteFile(f, big, (DWORD)bigN, &w, NULL); CloseHandle(f);
		free(big);
		snprintf(fp, sizeof fp, "%s\\patch-5.mpq", sd);
		CloseHandle(CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));
		snprintf(fp, sizeof fp, "%s\\Patch-LL.mpq", sd);
		CloseHandle(CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL));

		FreeLibrary(h);
		for (int w2 = 0; w2 < 300 && GetModuleHandleA("HDToggle.dll"); w2++)
			Sleep(50);
		build_world();
		DeleteFileA(ini);
		WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini);
		WritePrivateProfileStringA("Test", "datadir", sd, ini);
		write_server("play.octowow.st", "N'Zoth");
		h = LoadLibraryA(dll);
		g_nreg = 0;
		((void (*)(void))0x0048FE9C)();
		FakeL L;
		int done = 0;
		for (int pass = 0; pass < 3; pass++) {
			if (pass == 1) {
				/* önbellek kullanılıyor mu: aynı boyut ve zamanla kayıtlı değer okunur, dosya okunmaz */
				char v[160];
				GetPrivateProfileStringA("PackHashes", "patch-a.mpq", "", v, sizeof v, ini);
				char *sp = strchr(v, ' ');
				if (sp)
					strcpy(sp + 1, "1111111111111111111111111111111111111111111111111111111111111111");
				WritePrivateProfileStringA("PackHashes", "patch-a.mpq", v, ini);
			}
			if (pass == 2) {
				/* dosya değişince yeniden hesaplanır */
				snprintf(fp, sizeof fp, "%s\\patch-A.mpq", sd);
				f = CreateFileA(fp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, 0, NULL); WriteFile(f, "abd", 3, &w, NULL);
				FILETIME ft; GetSystemTimeAsFileTime(&ft); ft.dwLowDateTime += 50000000; SetFileTime(f, NULL, NULL, &ft);
				CloseHandle(f);
			}
			done = 0;
			for (int t = 0; t < 200 && !done; t++) {
				lua_reset(&L);
				if (t == 0 && pass > 0)
					push_n(&L, 1);
				int base = L.top;
				call_lua("HDPack_Scan", &L);
				done = L.v[base].type == 4 && !strcmp(L.v[base].s, "done");
				if (!done)
					Sleep(50);
			}
			int base = L.top - 4;
			int count = (int)L.v[base + 3].n;
			char got[8][2][96];
			int k = 0;
			for (int i = 1; i <= count && k < 8; i++, k++) {
				lua_reset(&L); push_n(&L, i);
				call_lua("HDPack_ScanEntry", &L);
				snprintf(got[k][0], sizeof got[k][0], "%s", L.v[1].type == 4 ? L.v[1].s : "?");
				snprintf(got[k][1], sizeof got[k][1], "%s", L.v[3].type == 4 ? L.v[3].s : "nil");
			}
			if (pass == 0) {
				CHECK(done && count == 3, "tarama biter; yalnız tek harfli paketler sayılır (patch-5, Patch-LL hariç)");
				CHECK(count == 3 && !strcmp(got[0][0], "patch-A.mpq") && !strcmp(got[1][0], "patch-b.mpq") && !strcmp(got[2][0], "patch-C.mpq"),
				      "paketler harf sırasıyla, büyük/küçük harf farketmeden");
				CHECK(count == 3 && !strcmp(got[0][1], "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),
				      "SHA-256 doğru (\"abc\")");
				CHECK(count == 3 && !strcmp(got[1][1], "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"),
				      "SHA-256 doğru (boş dosya)");
				CHECK(count == 3 && !strcmp(got[2][1], "4db207ba8abe00fd0ecb472e285b81ac7655ce21a44e0a308c0ada745476f683"),
				      "SHA-256 doğru (1,5 MB, okuma tamponundan büyük)");
			} else if (pass == 1) {
				CHECK(done && count == 3 && !strcmp(got[0][1], "1111111111111111111111111111111111111111111111111111111111111111"),
				      "değişmeyen paket önbellekten gelir (yeniden okunmaz)");
			} else {
				CHECK(done && count == 3 && !strcmp(got[0][1], "a52d159f262b2c6ddb724a61840befc36eb30c88877a4030b65cbe86298449c9"),
				      "değişen paket yeniden hesaplanır");
			}
		}
		lua_reset(&L); push_n(&L, 9);
		call_lua("HDPack_ScanEntry", &L);
		CHECK(L.v[1].type == 0, "olmayan sıra numarası nil döner");
	}
	printf("== teşhis günlüğü (debug)\n");
	{
		read_file(log, buf, sizeof buf);
		CHECK(!strstr(buf, "D1 ") && !strstr(buf, "D2 "), "debug kapalıyken ayrıntı satırı yazılmaz");
		FreeLibrary(h);
		for (int w2 = 0; w2 < 300 && GetModuleHandleA("HDToggle.dll"); w2++)
			Sleep(50);
		build_world();
		DeleteFileA(ini);
		WritePrivateProfileStringA("HDToggle", "updatecheck", "0", ini);
		WritePrivateProfileStringA("HDToggle", "debug", "2", ini);
		write_server("play.octowow.st", "N'Zoth");
		h = LoadLibraryA(dll);
		g_nreg = 0;
		((void (*)(void))0x0048FE9C)();
		FakeL L;
		set_pack("T", 1); set_pack("C", 1);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		build_textures();
		set_pack("T", 0);
		lua_reset(&L); call_lua("HDPack_Refresh", &L);
		read_file(log, buf, sizeof buf);
		CHECK(strstr(buf, "D1 debug level 2; this DLL:") && strstr(buf, "D1 module HDToggle.dll") && !strstr(buf, "WARNING: another HDToggle"),
		      "açılış: debug seviyesi, DLL yolu ve yüklü modüller; tek kopya için uyarı yok");
		CHECK(strstr(buf, "D1 refresh start (world): off before [], off now [T], changed [T]") != NULL,
		      "yenileme başı: önceki ve yeni seçim, değişen paketler");
		CHECK(strstr(buf, "D1 texture rebuilt Item\\Sword.blp: patch-T.mpq -> patch.MPQ, now 1024x512") != NULL,
		      "yenilenen doku: adı, eski ve yeni paketi");
		CHECK(strstr(buf, "D1 image re-decoded Creature\\Skin.blp: patch-T.mpq -> patch-C.mpq") != NULL,
		      "yeniden çözülen ten parçası: adı, paketleri, boyut ve biçim");
		CHECK(strstr(buf, "D1 refresh timing:") != NULL, "aşama süreleri yazılır");
		CHECK(strstr(buf, "D2 file ") != NULL, "debug=2: dosya yönlendirmeleri yazılır");
	}
	read_file(log, buf, sizeof buf);
	printf("\n-- HDToggle.log --\n%s", buf);
	printf("\n%d/%d test geçti\n", g_count - g_fail, g_count);
	return g_fail ? 1 : 0;
}
