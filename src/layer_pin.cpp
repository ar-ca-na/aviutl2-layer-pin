//----------------------------------------------------------------------------------
//  レイヤー固定 (layer_pin.aux2) for AviUtl ExEdit2
//  指定したレイヤーを、縦にスクロールしてもレイヤー編集の一番上に表示し続ける。
//  本体のレイヤー編集は子ウィンドウを持たないので、その上に自前の子ウィンドウを重ねる。
//  固定したレイヤーが本来の位置に見えている間は重ねない（一番上までスクロールすれば全レイヤーが見える）。
//----------------------------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string>
#include <vector>
#include <map>
#include <set>

#include "sdk/plugin2.h"
#include "sdk/logger2.h"

#define VERSION_TEXT L"v1.0.0"

static EDIT_HANDLE* g_edit = nullptr;
static LOG_HANDLE* g_log = nullptr;
static HWND g_host = nullptr;
static HWND g_ov = nullptr;
static HINSTANCE g_inst = nullptr;
static std::map<int, std::set<int>> g_pins;  // scene_id -> 固定レイヤー(0から)
static void pins_changed();
static bool is_pinned(int scene, int layer) {
	auto it = g_pins.find(scene);
	return it != g_pins.end() && it->second.count(layer) > 0;
}
// 固定のオン・オフを切り替える（メニューは 1 項目のチェックで表す）
static void toggle_pin(int scene, int layer) {
	if (layer < 0) return;
	auto& s = g_pins[scene];
	if (!s.erase(layer)) s.insert(layer);
	if (s.empty()) g_pins.erase(scene);
	pins_changed();
}
static bool g_test = false;        // 環境変数 LAYER_PIN_TEST があるときだけ、外からの試験用コマンドを受ける
static std::wstring g_dataDir;     // プラグインフォルダの親（style.conf の上書き・試験用ファイルの置き場）

//---------------------------------------------------------------------
//  ログ
//---------------------------------------------------------------------
static void dbg(const wchar_t* fmt, ...) {
	if (!g_test) return;
	wchar_t buf[1024];
	va_list ap; va_start(ap, fmt); _vsnwprintf(buf, 1023, fmt, ap); va_end(ap); buf[1023] = 0;
	std::wstring p = g_dataDir + L"\\layer_pin_test.log";
	FILE* f = _wfopen(p.c_str(), L"a, ccs=UTF-8");
	if (f) { fwprintf(f, L"%ls\n", buf); fclose(f); }
}

//---------------------------------------------------------------------
//  配色（本体の style.conf と、データフォルダの style.conf の上書きを読む）
//---------------------------------------------------------------------
struct Col { COLORREF c1, c2; BYTE a; };
static std::map<std::wstring, std::wstring> g_style;
static std::wstring g_fontFamily = L"Yu Gothic UI";
static int g_fontSize = 16;
static int g_headerWidth = 96;
static int g_layerHeight = 32;
static int g_scrollBar = 20;

static std::wstring trim(const std::wstring& s) {
	size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
	return a == std::wstring::npos ? L"" : s.substr(a, b - a + 1);
}

static void load_style_file(const std::wstring& path) {
	HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE) return;
	DWORD size = GetFileSize(h, nullptr), rd = 0;
	std::string buf(size, '\0');
	ReadFile(h, buf.data(), size, &rd, nullptr);
	CloseHandle(h);
	int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)rd, nullptr, 0);
	std::wstring w(n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)rd, w.data(), n);
	std::wstring sec;
	size_t pos = 0;
	while (pos < w.size()) {
		size_t e = w.find(L'\n', pos);
		if (e == std::wstring::npos) e = w.size();
		std::wstring line = trim(w.substr(pos, e - pos));
		pos = e + 1;
		if (line.empty() || line[0] == L';') continue;
		if (line[0] == L'[') { sec = line.substr(1, line.find(L']') - 1); continue; }
		size_t eq = line.find(L'=');
		if (eq == std::wstring::npos) continue;
		g_style[sec + L"." + trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
	}
}

static COLORREF hex_rgb(const std::wstring& s, BYTE* alpha) {
	unsigned v = wcstoul(s.substr(0, 6).c_str(), nullptr, 16);
	if (alpha) *alpha = s.size() >= 8 ? (BYTE)wcstoul(s.substr(6, 2).c_str(), nullptr, 16) : 255;
	return RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
}

static Col color(const wchar_t* key, COLORREF def) {
	Col c{def, def, 255};
	auto it = g_style.find(std::wstring(L"Color.") + key);
	if (it == g_style.end()) return c;
	std::wstring v = it->second;
	size_t comma = v.find(L',');
	c.c1 = hex_rgb(trim(v.substr(0, comma)), &c.a);
	c.c2 = comma == std::wstring::npos ? c.c1 : hex_rgb(trim(v.substr(comma + 1)), nullptr);
	return c;
}

static void load_style() {
	wchar_t exe[MAX_PATH];
	GetModuleFileNameW(nullptr, exe, MAX_PATH);
	std::wstring dir(exe);
	dir = dir.substr(0, dir.find_last_of(L'\\'));
	load_style_file(dir + L"\\style.conf");
	load_style_file(g_dataDir + L"\\style.conf");
	auto f = g_style.find(L"Font.DefaultFamily");
	if (f != g_style.end()) g_fontFamily = f->second;
	f = g_style.find(L"Font.LayerObject");
	if (f != g_style.end()) {
		std::wstring v = f->second;
		size_t comma = v.find(L',');
		g_fontSize = _wtoi(v.substr(0, comma).c_str());
		if (comma != std::wstring::npos) g_fontFamily = trim(v.substr(comma + 1));
	}
	f = g_style.find(L"Layout.ScrollBarSize");
	if (f != g_style.end()) g_scrollBar = _wtoi(f->second.c_str());
	f = g_style.find(L"Layout.LayerHeight");
	if (f != g_style.end()) g_layerHeight = _wtoi(f->second.c_str());
	f = g_style.find(L"Layout.LayerHeaderWidth");
	if (f != g_style.end()) g_headerWidth = _wtoi(f->second.c_str());
}

//---------------------------------------------------------------------
//  レイヤー編集の位置合わせ
//  pos_to_layer_frame でスクリーン座標→レイヤー・フレームを引き、表示中の並びを割り出す。
//---------------------------------------------------------------------
struct Geo {
	bool ok = false;
	int left = 0, right = 0;     // オブジェクト欄の左右（スクリーン座標、right は含まない）
	int top = 0, bottom = 0;     // レイヤー編集の上下
	int rowTop = 0, rowH = 0;    // 表示の先頭レイヤーの行
	int layer0 = 0;              // 表示の先頭レイヤー
	double x0 = 0, ppf = 1;      // フレーム f の左端 = x0 + (f - f0) * ppf
	int f0 = 0;
};
static Geo g_geo;

struct GeoKey {
	int layer_start, frame_start, frame_num, layer_num, scene;
	RECT client;
	bool operator==(const GeoKey& o) const {
		return layer_start == o.layer_start && frame_start == o.frame_start && frame_num == o.frame_num &&
			layer_num == o.layer_num && scene == o.scene && EqualRect(&client, &o.client);
	}
};
static GeoKey g_geoKey{};
static DWORD g_geoTime = 0;

static void calibrate(EDIT_SECTION* edit) {
	Geo g;
	RECT rc;
	GetClientRect(g_host, &rc);
	POINT o{0, 0};
	ClientToScreen(g_host, &o);
	int L = o.x, T = o.y, R = o.x + rc.right, B = o.y + rc.bottom;
	auto hit = [&](int x, int y, int* ly, int* fr) {
		int a, b;
		if (!ly) ly = &a;
		if (!fr) fr = &b;
		return edit->pos_to_layer_frame(x, y, ly, fr);
	};
	// 粗く走査して当たる範囲を探す
	const int step = 12;
	int minx = INT_MAX, maxx = INT_MIN, miny = INT_MAX, maxy = INT_MIN;
	for (int y = T + step / 2; y < B; y += step)
		for (int x = L + step / 2; x < R; x += step)
			if (hit(x, y, nullptr, nullptr)) {
				if (x < minx) minx = x;
				if (x > maxx) maxx = x;
				if (y < miny) miny = y;
				if (y > maxy) maxy = y;
			}
	if (minx == INT_MAX) { g_geo = g; return; }
	int cx = (minx + maxx) / 2, cy = (miny + maxy) / 2;
	// 端を 1px 単位で詰める
	int lo, hi;
	lo = L - 1; hi = minx; while (hi - lo > 1) { int m = (lo + hi) / 2; if (hit(m, cy, nullptr, nullptr)) hi = m; else lo = m; } g.left = hi;
	lo = maxx; hi = R; while (hi - lo > 1) { int m = (lo + hi) / 2; if (hit(m, cy, nullptr, nullptr)) lo = m; else hi = m; } g.right = hi;
	lo = T - 1; hi = miny; while (hi - lo > 1) { int m = (lo + hi) / 2; if (hit(cx, m, nullptr, nullptr)) hi = m; else lo = m; } g.top = hi;
	lo = maxy; hi = B; while (hi - lo > 1) { int m = (lo + hi) / 2; if (hit(cx, m, nullptr, nullptr)) lo = m; else hi = m; } g.bottom = hi;
	if (g_test) {
		std::wstring t; int pl = INT_MIN, l2, f2;
		for (int y = g.top - 3; y < g.bottom + 3; y++) {
			bool h = hit(cx, y, &l2, &f2);
			int k = h ? l2 : -999;
			if (k != pl) { t += std::to_wstring(y) + L":" + std::to_wstring(k) + L" "; pl = k; }
		}
		dbg(L"edges L=%d R=%d T=%d B=%d cx=%d rows %ls", g.left, g.right, g.top, g.bottom, cx, t.c_str());
		t.clear(); pl = INT_MIN;
		for (int x = L; x < R; x += 1) {
			bool h = hit(x, g.top + 40, &l2, &f2);
			int k = h ? f2 : -999;
			if (k != pl && (k == -999 || pl == -999 || pl == INT_MIN)) { t += std::to_wstring(x) + L":" + std::to_wstring(k) + L" "; }
			pl = k;
		}
		dbg(L"xscan %ls", t.c_str());
	}
	// 行：時間ゲージ等の上でも「先頭レイヤーより上のレイヤー」として当たるので、
	// 先頭レイヤー+1 と +2 の境目から行の高さと先頭行の位置を出す
	int s0 = edit->info->display_layer_start, ly = 0, y1 = -1, y2 = -1;
	for (int y = g.top; y < g.bottom; y++) {
		if (!hit(cx, y, &ly, nullptr)) continue;
		if (y1 < 0 && ly >= s0 + 1) y1 = y;
		if (y2 < 0 && ly >= s0 + 2) { y2 = y; break; }
	}
	if (y1 < 0) { g_geo = g; return; }
	g.rowH = y2 > 0 ? y2 - y1 : g_layerHeight;
	g.rowTop = y1 - g.rowH;
	g.layer0 = s0;
	// フレーム：先頭行の中ほどを左から 1px ずつ
	int ym = g.rowTop + g.rowH / 2;
	int ntrans = 0;
	int fprev = INT_MIN, fr = 0;
	int xa = -1, fa = 0, xb = -1, fb = 0;
	// レイヤー名の欄のフレームは直線に乗らないので、オブジェクト欄だけを見る
	for (int x = g.left + g_headerWidth; x < g.right - g_scrollBar; x++) {
		if (!hit(x, ym, nullptr, &fr)) continue;
		if (fprev != INT_MIN && fr != fprev) {
			if (xa < 0) { xa = x; fa = fr; }
			xb = x; fb = fr; ntrans++;
		}
		fprev = fr;
	}
	dbg(L"frames ym=%d trans=%d xa=%d fa=%d xb=%d fb=%d", ym, ntrans, xa, fa, xb, fb);
	if (xa < 0 || xb == xa) {
		// 拡大しすぎて 1 フレームが画面より広いときは、表示開始フレームと 1 フレームの幅から近似する
		g_geo = g; g_geo.ok = false; return;
	}
	g.ppf = double(xb - xa) / double(fb - fa);
	g.x0 = xa; g.f0 = fa;
	// フレーム境界が切り上げで出ている分を最小二乗ではなく中央で合わせる（誤差は 1px 未満）
	// 右端の縦スクロールバーの上でも当たるので、その分を除く
	g.right -= g_scrollBar;
	g.ok = g.rowH > 2 && g.right > g.left + g_headerWidth;
	g_geo = g;
	dbg(L"calib area x=%d..%d y=%d..%d row0=%d top=%d h=%d f0=%d x0=%.1f ppf=%.4f",
		g.left, g.right, g.top, g.bottom, g.layer0, g.rowTop, g.rowH, g.f0, g.x0, g.ppf);
}

static double frame_to_x(double f) { return g_geo.x0 + (f - g_geo.f0) * g_geo.ppf; }
static double x_to_frame(double x) { return g_geo.f0 + (x - g_geo.x0) / g_geo.ppf; }

//---------------------------------------------------------------------
//  固定レイヤーの中身
//---------------------------------------------------------------------
struct Obj {
	OBJECT_HANDLE h;
	int start, end;
	std::wstring name;
	int kind;      // 0=映像 1=音声 2=制御 3=映像フィルタ 4=音声フィルタ 5=シーンチェンジ
	bool sel, focus;
	bool operator==(const Obj& o) const {
		return h == o.h && start == o.start && end == o.end && name == o.name && kind == o.kind && sel == o.sel && focus == o.focus;
	}
};
struct Row {
	int layer = -1;
	std::wstring layerName;
	bool layerEnable = true, layerLock = false;
	std::vector<Obj> objs;
	bool operator==(const Row& o) const {
		return layer == o.layer && layerName == o.layerName && layerEnable == o.layerEnable && layerLock == o.layerLock && objs == o.objs;
	}
};
struct View {
	bool show = false;
	std::vector<Row> rows;  // 上から順に重ねる固定レイヤー
	int cursor = 0;
	int rangeStart = -1, rangeEnd = -1;
	RECT rc{};  // 重ねる窓の位置（ホストのクライアント座標）
	bool operator==(const View& o) const {
		return show == o.show && rows == o.rows && cursor == o.cursor && rangeStart == o.rangeStart && rangeEnd == o.rangeEnd &&
			EqualRect(&rc, &o.rc);
	}
};
static View g_view;

//---------------------------------------------------------------------
//  固定レイヤーのレイヤー名の太い枠（本来の位置に見えている行）
//  本体の描いた名前を隠さないよう、細い子ウィンドウ 4 本で枠を作る。
//  マウスを本体へ通すため無効化しておく（無効な子ウィンドウの上のクリックは親へ届く。HTTRANSPARENT では届かなかった）
//---------------------------------------------------------------------
static const int FRAME_WIDTH = 3;
static std::vector<HWND> g_bars;
static std::vector<RECT> g_frames;

static void update_frames(const std::vector<RECT>& frames) {
	bool same = frames.size() == g_frames.size();
	for (size_t i = 0; same && i < frames.size(); i++) same = EqualRect(&frames[i], &g_frames[i]);
	if (same) return;
	g_frames = frames;
	while (g_bars.size() < frames.size() * 4)
		g_bars.push_back(CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinFrame", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, 0, 0, 1, 1, g_host, nullptr, g_inst, nullptr));
	const int w = FRAME_WIDTH;
	for (size_t i = 0; i < g_bars.size(); i++) {
		size_t f = i / 4;
		if (f >= frames.size()) { ShowWindow(g_bars[i], SW_HIDE); continue; }
		const RECT& r = frames[f];
		RECT b;
		switch (i % 4) {
		case 0: b = {r.left, r.top, r.right, r.top + w}; break;
		case 1: b = {r.left, r.bottom - w, r.right, r.bottom}; break;
		case 2: b = {r.left, r.top, r.left + w, r.bottom}; break;
		default: b = {r.right - w, r.top, r.right, r.bottom}; break;
		}
		SetWindowPos(g_bars[i], HWND_TOP, b.left, b.top, b.right - b.left, b.bottom - b.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	}
}

static std::map<std::wstring, std::pair<int, int>> g_effectTypes;  // エフェクト名 -> (種別, フラグ)
static bool g_effectTypesLoaded = false;

static int kind_of(const std::wstring& effect) {
	auto it = g_effectTypes.find(effect);
	if (it == g_effectTypes.end()) return 0;
	int type = it->second.first, flag = it->second.second;
	bool video = flag & EDIT_HANDLE::EFFECT_FLAG_VIDEO, audio = flag & EDIT_HANDLE::EFFECT_FLAG_AUDIO;
	switch (type) {
	case EDIT_HANDLE::EFFECT_TYPE_INPUT: return (!video && audio) ? 1 : 0;
	case EDIT_HANDLE::EFFECT_TYPE_CONTROL: return 2;
	case EDIT_HANDLE::EFFECT_TYPE_FILTER: return (!video && audio) ? 4 : 3;
	case EDIT_HANDLE::EFFECT_TYPE_TRANSITION: return 5;
	}
	return 0;
}

static std::wstring utf8w(LPCSTR s) {
	if (!s) return L"";
	int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
	std::wstring w(n > 0 ? n - 1 : 0, L'\0');
	if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
	return w;
}

// 名前を付けていないオブジェクトは、本体と同じく中身で呼ぶ（テキストは本文、図形は種類、ファイルはファイル名）
static std::wstring default_name(EDIT_SECTION* edit, OBJECT_HANDLE h, const std::wstring& effect) {
	if (effect.empty()) return effect;
	std::wstring v = utf8w(edit->get_object_item_value(h, effect.c_str(), L"テキスト"));
	if (!v.empty()) {
		size_t nl = v.find_first_of(L"\r\n");
		if (nl != std::wstring::npos) v = v.substr(0, nl);
		// エイリアス形式の改行は \\n の 2 文字
		size_t esc = v.find(L"\\n");
		if (esc != std::wstring::npos) v = v.substr(0, esc);
		return v;
	}
	v = utf8w(edit->get_object_item_value(h, effect.c_str(), L"図形の種類"));
	if (!v.empty()) return v;
	v = utf8w(edit->get_object_item_value(h, effect.c_str(), L"ファイル"));
	if (!v.empty()) return v.substr(v.find_last_of(L"\\/") + 1);
	return effect;
}

struct ReadCtx { View v; int frameEnd; };

static void read_objects(void* p, EDIT_SECTION* edit) {
	ReadCtx* c = (ReadCtx*)p;
	OBJECT_HANDLE focus = edit->get_focus_object();
	int nsel = edit->get_selected_object_num();
	std::vector<OBJECT_HANDLE> sel;
	for (int i = 0; i < nsel; i++) sel.push_back(edit->get_selected_object(i));
	for (auto& v : c->v.rows) {
		auto nm = edit->get_layer_name(v.layer);
		v.layerName = nm ? nm : L"";
		v.layerEnable = edit->get_layer_enable(v.layer);
		v.layerLock = edit->get_layer_lock(v.layer);
		int f = 0;
		for (int guard = 0; guard < 100000; guard++) {
			OBJECT_HANDLE h = edit->find_object(v.layer, f);
			if (!h) break;
			OBJECT_LAYER_FRAME lf = edit->get_object_layer_frame(h);
			if (lf.end < f) break;
			f = lf.end + 1;
			if (lf.start > c->frameEnd) break;
			Obj o{};
			o.h = h; o.start = lf.start; o.end = lf.end;
			LPCWSTR on = edit->get_object_name(h);
			EFFECT_HANDLE eff[1] = {};
			std::wstring first;
			if (edit->get_effect_list(h, eff, 1) > 0 && eff[0]) {
				LPCWSTR en = edit->get_effect_name(eff[0]);
				if (en) first = en;
			}
			o.name = on ? std::wstring(on) : default_name(edit, h, first);
			o.kind = kind_of(first);
			o.focus = h == focus;
			o.sel = o.focus;
			for (auto s : sel) if (s == h) o.sel = true;
			v.objs.push_back(o);
			if (g_test) dbg(L"obj L%d %d..%d %ls kind=%d", v.layer, o.start, o.end, o.name.c_str(), o.kind);
		}
	}
}

static void calib_cb(void*, EDIT_SECTION* edit) { calibrate(edit); }

//---------------------------------------------------------------------
//  試験用（LAYER_PIN_TEST があるときだけ）：データフォルダの layer_pin_cmd.txt を 1 行ずつ実行
//---------------------------------------------------------------------
static void run_test_commands() {
	std::wstring p = g_dataDir + L"\\layer_pin_cmd.txt";
	FILE* f = _wfopen(p.c_str(), L"r, ccs=UTF-8");
	if (!f) return;
	std::vector<std::wstring> lines;
	wchar_t buf[512];
	while (fgetws(buf, 512, f)) lines.push_back(trim(buf));
	fclose(f);
	DeleteFileW(p.c_str());
	for (auto& line : lines) {
		int a = 0, b = 0;
		if (swscanf(line.c_str(), L"scroll %d %d", &a, &b) == 2) {
			struct P { int a, b; } prm{a, b};
			g_edit->call_edit_section_param(&prm, [](void* q, EDIT_SECTION* e) { auto r = (P*)q; e->set_display_layer_frame(r->a, r->b); });
		} else if (swscanf(line.c_str(), L"pin %d", &a) == 1) {
			EDIT_INFO info{}; g_edit->get_edit_info(&info, sizeof(info));
			if (a < 0) g_pins.erase(info.scene_id); else toggle_pin(info.scene_id, a);
		}
		dbg(L"cmd: %ls", line.c_str());
	}
}

//---------------------------------------------------------------------
//  毎回の更新
//---------------------------------------------------------------------
static void tick() {
	if (!g_host) {
		g_host = g_edit->get_host_app_window();
		if (!g_host) return;
		SetWindowLongPtrW(g_ov, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
		SetParent(g_ov, g_host);
		dbg(L"host=%p style=%08lx", g_host, (unsigned long)GetWindowLongPtrW(g_host, GWL_STYLE));
	}
	if (!g_effectTypesLoaded) {
		g_effectTypesLoaded = true;
		g_edit->enum_effect_name(nullptr, [](void*, LPCWSTR name, int type, int flag) { g_effectTypes[name] = {type, flag}; });
	}
	if (g_test) run_test_commands();

	View v;
	EDIT_INFO info{};
	g_edit->get_edit_info(&info, sizeof(info));
	auto it = g_pins.find(info.scene_id);
	bool any = it != g_pins.end() && !IsIconic(g_host) && !it->second.empty();
	bool geoOk = false;
	if (any) {
		RECT rc; GetClientRect(g_host, &rc);
		GeoKey k{info.display_layer_start, info.display_frame_start, info.display_frame_num, info.display_layer_num, info.scene_id, rc};
		DWORD now = GetTickCount();
		if (!(k == g_geoKey) || now - g_geoTime > 1000) {
			// ドラッグ編集中などは失敗するので、前回の位置のまま
			if (g_edit->call_edit_section_param(nullptr, calib_cb)) { g_geoKey = k; g_geoTime = now; }
		}
		geoOk = g_geo.ok && g_geo.layer0 == info.display_layer_start;
	}
	bool want = geoOk && info.display_layer_start > *it->second.begin();
	if (want) {
		// 本来の位置が重ねた行の下に隠れる（か画面より上にある）固定レイヤーを、上から積む。
		// 少なくとも 1 行は本来の行が見えるように残す
		int maxRows = (g_geo.bottom - g_geo.rowTop) / g_geo.rowH - 1;
		for (int layer : it->second) {
			if ((int)v.rows.size() >= maxRows) break;
			if (layer - info.display_layer_start < (int)v.rows.size()) { Row r; r.layer = layer; v.rows.push_back(r); }
		}
		if (v.rows.empty()) want = false;
	}
	if (want) {
		int n = (int)v.rows.size();
		POINT a{g_geo.left, g_geo.rowTop}, b{g_geo.right, g_geo.rowTop + g_geo.rowH * n};
		ScreenToClient(g_host, &a);
		ScreenToClient(g_host, &b);
		v.rc = {a.x, a.y, b.x, b.y};
		v.cursor = info.frame;
		v.rangeStart = info.select_range_start;
		v.rangeEnd = info.select_range_end;
		ReadCtx c{v, (int)ceil(x_to_frame(g_geo.right)) + 1};
		g_edit->call_read_section_param(&c, read_objects);
		v = c.v;
		v.show = true;
	}
	if (g_test) {
		static std::wstring last;
		wchar_t s[256];
		std::wstring rows;
		for (auto& r : v.rows) rows += std::to_wstring(r.layer) + L"(" + std::to_wstring(r.objs.size()) + L") ";
		swprintf(s, 256, L"tick dls=%d geo.ok=%d layer0=%d show=%d rc=%ld,%ld,%ld,%ld rows=%ls", info.display_layer_start,
			g_geo.ok, g_geo.layer0, v.show, v.rc.left, v.rc.top, v.rc.right, v.rc.bottom, rows.c_str());
		if (last != s) { last = s; dbg(L"%ls", s); }
	}
	// 本来の位置に見えている固定レイヤーは、レイヤー名の枠を太くする
	std::vector<RECT> frames;
	if (geoOk) {
		for (int layer : it->second) {
			int k = layer - info.display_layer_start;
			int top = g_geo.rowTop + k * g_geo.rowH;
			if (k < (int)v.rows.size() || top + g_geo.rowH > g_geo.bottom) continue;
			POINT a{g_geo.left, top}, b{g_geo.left + g_headerWidth, top + g_geo.rowH};
			ScreenToClient(g_host, &a);
			ScreenToClient(g_host, &b);
			frames.push_back({a.x, a.y, b.x, b.y});
		}
	}
	update_frames(frames);
	if (v == g_view) return;
	bool moved = !EqualRect(&v.rc, &g_view.rc);
	g_view = v;
	if (!v.show) {
		if (IsWindowVisible(g_ov)) ShowWindow(g_ov, SW_HIDE);
		return;
	}
	if (moved || !IsWindowVisible(g_ov))
		SetWindowPos(g_ov, HWND_TOP, v.rc.left, v.rc.top, v.rc.right - v.rc.left, v.rc.bottom - v.rc.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(g_ov, nullptr, FALSE);
}

//---------------------------------------------------------------------
//  描画
//---------------------------------------------------------------------
static void fill(HDC dc, RECT r, COLORREF c) {
	HBRUSH b = CreateSolidBrush(c);
	FillRect(dc, &r, b);
	DeleteObject(b);
}

static COLORREF blend(COLORREF a, COLORREF b, int alpha) {
	return RGB((GetRValue(a) * (255 - alpha) + GetRValue(b) * alpha) / 255,
		(GetGValue(a) * (255 - alpha) + GetGValue(b) * alpha) / 255,
		(GetBValue(a) * (255 - alpha) + GetBValue(b) * alpha) / 255);
}

static void vgrad(HDC dc, RECT r, COLORREF top, COLORREF bottom) {
	TRIVERTEX v[2] = {
		{r.left, r.top, (COLOR16)(GetRValue(top) << 8), (COLOR16)(GetGValue(top) << 8), (COLOR16)(GetBValue(top) << 8), 0},
		{r.right, r.bottom, (COLOR16)(GetRValue(bottom) << 8), (COLOR16)(GetGValue(bottom) << 8), (COLOR16)(GetBValue(bottom) << 8), 0},
	};
	GRADIENT_RECT gr{0, 1};
	GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_V);
}

// ドラッグ中の見た目
static struct Drag {
	bool down = false, moving = false;
	OBJECT_HANDLE h = nullptr;
	int start = 0, len = 0;
	POINT p0{};
	int dx = 0;
} g_drag;

static int obj_left(int ovLeftScreen, int start) { return (int)floor(frame_to_x(start)) - ovLeftScreen; }

static void paint(HWND hwnd) {
	PAINTSTRUCT ps;
	HDC wdc = BeginPaint(hwnd, &ps);
	RECT rc; GetClientRect(hwnd, &rc);
	HDC dc = CreateCompatibleDC(wdc);
	HBITMAP bmp = CreateCompatibleBitmap(wdc, rc.right, rc.bottom);
	HGDIOBJ oldBmp = SelectObject(dc, bmp);
	// 本体のレイヤー編集は拡大率に関係なく style.conf の px のまま描かれるので、拡大率を掛けない
	HFONT font = CreateFontW(-g_fontSize, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, g_fontFamily.c_str());
	HGDIOBJ oldFont = SelectObject(dc, font);
	SetBkMode(dc, TRANSPARENT);

	POINT org{0, 0};
	ClientToScreen(hwnd, &org);
	// pos_to_layer_frame はレイヤー名の欄でも当たるので、欄の幅は style.conf から取る（本体は拡大率を掛けない）
	int areaL = g_geo.left + g_headerWidth - org.x;  // オブジェクト欄の左端（この窓の座標）
	Col cLayer = color(L"Layer", RGB(0x40, 0x40, 0x40));
	Col cLayerDis = color(L"LayerDisable", RGB(0x34, 0x34, 0x34));
	Col cHeader = color(L"LayerHeader", RGB(0x4a, 0x4a, 0x4a));
	Col cText = color(L"Text", RGB(255, 255, 255));
	Col cTextDis = color(L"TextDisable", RGB(0x90, 0x90, 0x90));
	Col cFocus = color(L"ObjectFocus", RGB(0xc0, 0xc0, 0xc0));
	Col cCursor = color(L"FrameCursor", RGB(0xc8, 0x30, 0x30));
	Col cRange = color(L"LayerRange", RGB(0x70, 0x70, 0x70));
	Col cPin = color(L"BorderFocus", RGB(0x80, 0x80, 0xe0));
	const View& v = g_view;
	static const wchar_t* kinds[][2] = {
		{L"ObjectVideo", L"ObjectVideoSelect"}, {L"ObjectAudio", L"ObjectAudioSelect"},
		{L"ObjectControl", L"ObjectControlSelect"}, {L"ObjectVideoFilter", L"ObjectVideoFilterSelect"},
		{L"ObjectAudioFilter", L"ObjectAudioFilterSelect"}, {L"ObjectVideoEffect", L"ObjectVideoEffectSelect"},
	};
	const int pad = 1, rh = g_geo.rowH;
	auto draw_obj = [&](const Obj& o, RECT r, RECT clip, bool enable, bool selected) {
		Col base = color(kinds[o.kind][0], RGB(0x30, 0x40, 0xe0));
		if (selected) { Col sc = color(kinds[o.kind][1], base.c1); vgrad(dc, r, sc.c1, sc.c1); }
		else vgrad(dc, r, base.c1, base.c2);
		if (!enable) {
			// 非表示レイヤーは暗く
			BLENDFUNCTION bf{AC_SRC_OVER, 0, 128, 0};
			HDC tdc = CreateCompatibleDC(dc); HBITMAP tb = CreateCompatibleBitmap(dc, 1, 1); HGDIOBJ ob = SelectObject(tdc, tb);
			SetPixel(tdc, 0, 0, cLayerDis.c1);
			AlphaBlend(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, tdc, 0, 0, 1, 1, bf);
			SelectObject(tdc, ob); DeleteObject(tb); DeleteDC(tdc);
		}
		if (o.focus || (g_drag.moving && o.h == g_drag.h)) {
			HBRUSH b = CreateSolidBrush(cFocus.c1);
			FrameRect(dc, &r, b);
			DeleteObject(b);
		}
		RECT tr = clip;
		tr.left = (r.left < areaL ? areaL : r.left) + 4;
		tr.right -= 2;
		SetTextColor(dc, enable ? cText.c1 : cTextDis.c1);
		DrawTextW(dc, o.name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
	};

	for (size_t i = 0; i < v.rows.size(); i++) {
		const Row& row = v.rows[i];
		int top = (int)i * rh, bottom = top + rh;
		// 背景
		RECT body{areaL, top, rc.right, bottom};
		COLORREF bg = row.layerEnable ? cLayer.c1 : cLayerDis.c1;
		fill(dc, body, bg);
		if (v.rangeStart >= 0 && v.rangeEnd >= v.rangeStart) {
			RECT r{(int)floor(frame_to_x(v.rangeStart)) - org.x, top, (int)floor(frame_to_x(v.rangeEnd + 1)) - org.x, bottom};
			IntersectRect(&r, &r, &body);
			fill(dc, r, blend(bg, cRange.c1, cRange.a));
		}
		// オブジェクト（ドラッグ中のものは後で上に描く）
		for (auto& o : row.objs) {
			if (g_drag.moving && o.h == g_drag.h) continue;
			RECT r{obj_left(org.x, o.start), top + pad, obj_left(org.x, o.end + 1), bottom - pad};
			RECT clip;
			if (IntersectRect(&clip, &r, &body)) draw_obj(o, r, clip, row.layerEnable, o.sel);
		}
		// 現在フレームのカーソル
		int cx = (int)floor(frame_to_x(v.cursor)) - org.x;
		if (cx >= areaL && cx < rc.right) fill(dc, RECT{cx, top, cx + 1, bottom}, cCursor.c1);
		// レイヤー名の欄
		if (areaL > 0) {
			fill(dc, RECT{0, top, areaL, bottom}, cHeader.c1);
			int w = FRAME_WIDTH;
			fill(dc, RECT{0, top, areaL, top + w}, cPin.c1);
			fill(dc, RECT{0, bottom - w, areaL, bottom}, cPin.c1);
			fill(dc, RECT{0, top, w, bottom}, cPin.c1);
			fill(dc, RECT{areaL - w, top, areaL, bottom}, cPin.c1);
			wchar_t label[300];
			if (row.layerName.empty()) swprintf(label, 300, L"Layer%d", row.layer + 1);
			else swprintf(label, 300, L"%ls", row.layerName.c_str());
			RECT tr{8, top, areaL - 4, bottom};
			SetTextColor(dc, row.layerEnable ? cText.c1 : cTextDis.c1);
			DrawTextW(dc, label, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
		}
		// 行の境目
		if (i + 1 < v.rows.size()) fill(dc, RECT{0, bottom - 1, rc.right, bottom}, cHeader.c1);
	}
	// ドラッグ中のオブジェクト（行の中を左右に動かす。別の行へ出たら、離した先へ移す）
	if (g_drag.moving) {
		for (size_t i = 0; i < v.rows.size(); i++)
			for (auto& o : v.rows[i].objs) if (o.h == g_drag.h) {
				int top = (int)i * rh;
				RECT r{obj_left(org.x, o.start) + g_drag.dx, top + pad, obj_left(org.x, o.end + 1) + g_drag.dx, top + rh - pad};
				RECT body{areaL, top, rc.right, top + rh}, clip;
				if (IntersectRect(&clip, &r, &body)) draw_obj(o, r, clip, true, true);
			}
	}
	// 下端の線：固定の行だと分かるように
	fill(dc, RECT{0, rc.bottom - 2, rc.right, rc.bottom}, cPin.c1);

	BitBlt(wdc, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
	SelectObject(dc, oldFont);
	DeleteObject(font);
	SelectObject(dc, oldBmp);
	DeleteObject(bmp);
	DeleteDC(dc);
	EndPaint(hwnd, &ps);
}

//---------------------------------------------------------------------
//  操作
//---------------------------------------------------------------------
static int hit_row(int y) {
	if (g_geo.rowH <= 0 || y < 0) return -1;
	int i = y / g_geo.rowH;
	return i < (int)g_view.rows.size() ? i : -1;
}

static const Obj* hit_object(int x, int y) {
	int i = hit_row(y);
	if (i < 0) return nullptr;
	POINT org{0, 0}; ClientToScreen(g_ov, &org);
	for (auto& o : g_view.rows[i].objs)
		if (x >= obj_left(org.x, o.start) && x < obj_left(org.x, o.end + 1)) return &o;
	return nullptr;
}

// layer < 0 なら、離した位置のレイヤー（本来の行）へ
struct DropCtx { OBJECT_HANDLE h; int frame; POINT screen; bool toOther; bool ok; int layer; };

static void drop_cb(void* p, EDIT_SECTION* edit) {
	DropCtx* c = (DropCtx*)p;
	int layer = c->layer, fr = 0;
	if (layer < 0) {
		if (!edit->pos_to_layer_frame(c->screen.x, c->screen.y, &layer, &fr)) { c->ok = false; return; }
	}
	c->layer = layer;
	c->ok = edit->move_object(c->h, layer, c->frame < 0 ? 0 : c->frame);
	if (c->ok) edit->set_focus_object(c->h);
}

static void pins_changed() {
	g_geoTime = 0;
	if (g_ov) PostMessageW(g_ov, WM_TIMER, 1, 0);
}

static LRESULT CALLBACK ov_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_TIMER:
		tick();
		return 0;
	case WM_PAINT:
		paint(hwnd);
		return 0;
	case WM_ERASEBKGND:
		return 1;
	case WM_SETCURSOR:
		SetCursor(LoadCursorW(nullptr, IDC_ARROW));
		return TRUE;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_LBUTTONDOWN: {
		int x = GET_X_LPARAM(lp);
		const Obj* o = hit_object(x, GET_Y_LPARAM(lp));
		int row = hit_row(GET_Y_LPARAM(lp));
		dbg(L"lbuttondown x=%d row=%d hit=%d", x, row, o != nullptr);
		if (row < 0) return 0;
		g_drag = Drag{};
		POINT s{x, GET_Y_LPARAM(lp)};
		ClientToScreen(hwnd, &s);
		if (o) {
			Obj hit = *o;
			// 編集セクションの後で掴む（先に掴むと、本体の処理でキャプチャが外れる）
			g_edit->call_edit_section_param(hit.h, [](void* q, EDIT_SECTION* e) { e->set_focus_object((OBJECT_HANDLE)q); });
			if (!g_view.rows[row].layerLock) {
				g_drag.down = true; g_drag.h = hit.h; g_drag.start = hit.start; g_drag.len = hit.end - hit.start;
				g_drag.p0 = s;
				SetCapture(hwnd);
				dbg(L"setcapture -> GetCapture=%p self=%p", (void*)GetCapture(), (void*)hwnd);
			}
		} else {
			struct P { int layer, frame; } prm{g_view.rows[row].layer, (int)floor(x_to_frame(s.x))};
			g_edit->call_edit_section_param(&prm, [](void* q, EDIT_SECTION* e) { auto r = (P*)q; e->set_cursor_layer_frame(r->layer, r->frame < 0 ? 0 : r->frame); });
		}
		return 0;
	}
	case WM_MOUSEMOVE:
		if (g_drag.down && !(wp & MK_LBUTTON)) { g_drag = Drag{}; InvalidateRect(hwnd, nullptr, FALSE); }
		if (g_drag.down) {
			POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
			ClientToScreen(hwnd, &s);
			int dx = s.x - g_drag.p0.x, dy = s.y - g_drag.p0.y;
			if (!g_drag.moving && (abs(dx) > GetSystemMetrics(SM_CXDRAG) || abs(dy) > GetSystemMetrics(SM_CYDRAG))) g_drag.moving = true;
			if (g_drag.moving) {
				int df = (int)lround(dx / g_geo.ppf);
				g_drag.dx = (int)lround(df * g_geo.ppf);
				RECT rc; GetClientRect(hwnd, &rc);
				POINT c{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
				SetCursor(LoadCursorW(nullptr, PtInRect(&rc, c) ? IDC_SIZEWE : IDC_SIZEALL));
				InvalidateRect(hwnd, nullptr, FALSE);
			}
		}
		return 0;
	case WM_LBUTTONUP:
		if (g_drag.down) {
			ReleaseCapture();
			if (g_drag.moving) {
				POINT s{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
				ClientToScreen(hwnd, &s);
				// 重ねた行の中なら、その行の固定レイヤーへ。外なら本来の行へ
				int row = hit_row(GET_Y_LPARAM(lp));
				int df = (int)lround((s.x - g_drag.p0.x) / g_geo.ppf);
				DropCtx c{g_drag.h, g_drag.start + df, s, row < 0, false, row < 0 ? -1 : g_view.rows[row].layer};
				g_edit->call_edit_section_param(&c, drop_cb);
				dbg(L"drop frame=%d other=%d ok=%d layer=%d", c.frame, c.toOther, c.ok, c.layer);
				if (!c.ok) MessageBeep(MB_ICONWARNING);
			}
			g_drag = Drag{};
			InvalidateRect(hwnd, nullptr, FALSE);
			g_geoTime = 0;
			tick();
		}
		return 0;
	case WM_CAPTURECHANGED:
		// 捕捉が外れても、ボタンが離れるまではドラッグを続ける（離れていれば次の WM_MOUSEMOVE で取り消す）
		dbg(L"capturechanged to=%p down=%d", (void*)lp, g_drag.down);
		return 0;
	case WM_RBUTTONUP: {
		int row = hit_row(GET_Y_LPARAM(lp));
		if (row < 0) return 0;
		int layer = g_view.rows[row].layer;
		HMENU m = CreatePopupMenu();
		AppendMenuW(m, MF_STRING | MF_CHECKED, 1, L"このレイヤーを一番上に固定");
		POINT s; GetCursorPos(&s);
		int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, s.x, s.y, 0, hwnd, nullptr);
		DestroyMenu(m);
		if (cmd == 1) {
			EDIT_INFO info{}; g_edit->get_edit_info(&info, sizeof(info));
			toggle_pin(info.scene_id, layer);
		}
		return 0;
	}
	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		// スクロール・拡大はレイヤー編集に任せる
		return SendMessageW(g_host, msg, wp, lp);
	}
	return DefWindowProcW(hwnd, msg, wp, lp);
}

//---------------------------------------------------------------------
//  メニュー
//---------------------------------------------------------------------
static int menu_layer(EDIT_SECTION* edit) {
	int layer = -1, frame = 0;
	if (!edit->get_mouse_layer_frame(&layer, &frame)) layer = edit->info->layer;
	return layer;
}
static void menu_toggle(EDIT_SECTION* edit) { toggle_pin(edit->info->scene_id, menu_layer(edit)); }

//---------------------------------------------------------------------
//  レイヤー名の欄の右クリックメニューに項目を足す
//  SDK ではこのメニューに足せないので、本体（aviutl2.exe）が呼ぶ TrackPopupMenu を差し替える。
//  「レイヤー名を変更」を含むメニューだけを対象にし、出し終えたら足した項目を消す。
//---------------------------------------------------------------------
typedef BOOL (WINAPI* TrackPopupMenu_t)(HMENU, UINT, int, int, int, HWND, const RECT*);
static TrackPopupMenu_t g_origTrack = nullptr;
static ULONG_PTR* g_iatSlot = nullptr;
static const UINT ID_PIN = 0x7E10;

static bool menu_has(HMENU m, const wchar_t* text) {
	int n = GetMenuItemCount(m);
	for (int i = 0; i < n; i++) {
		wchar_t b[256];
		if (GetMenuStringW(m, i, b, 256, MF_BYPOSITION) > 0 && wcsstr(b, text)) return true;
	}
	return false;
}

static BOOL WINAPI hook_TrackPopupMenu(HMENU m, UINT flags, int x, int y, int r, HWND h, const RECT* rc) {
	if (!g_edit || !menu_has(m, L"レイヤー名を変更")) return g_origTrack(m, flags, x, y, r, h, rc);
	// 右クリックで選択レイヤーがそのレイヤーに移っている
	EDIT_INFO info{};
	g_edit->get_edit_info(&info, sizeof(info));
	bool pinned = is_pinned(info.scene_id, info.layer);
	dbg(L"header menu layer=%d pinned=%d flags=%x", info.layer, pinned, flags);
	int base = GetMenuItemCount(m);
	AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(m, MF_STRING | (pinned ? MF_CHECKED : 0), ID_PIN, L"このレイヤーを一番上に固定");
	UINT ret = (UINT)g_origTrack(m, flags | TPM_RETURNCMD, x, y, r, h, rc);
	while (GetMenuItemCount(m) > base) DeleteMenu(m, base, MF_BYPOSITION);
	if (ret == ID_PIN) {
		toggle_pin(info.scene_id, info.layer);
		return (flags & TPM_RETURNCMD) ? 0 : TRUE;
	}
	if (flags & TPM_RETURNCMD) return (BOOL)ret;
	// 本体は選んだ項目を WM_COMMAND で受け取る作り
	if (ret) PostMessageW(h, WM_COMMAND, ret, 0);
	return ret != 0;
}

static bool hook_iat() {
	BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
	auto dos = (IMAGE_DOS_HEADER*)base;
	auto nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
	auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	if (!dir.VirtualAddress) return false;
	for (auto imp = (IMAGE_IMPORT_DESCRIPTOR*)(base + dir.VirtualAddress); imp->Name; imp++) {
		if (_stricmp((char*)(base + imp->Name), "USER32.dll") != 0) continue;
		auto names = (IMAGE_THUNK_DATA*)(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk : imp->FirstThunk));
		auto funcs = (IMAGE_THUNK_DATA*)(base + imp->FirstThunk);
		for (; names->u1.AddressOfData; names++, funcs++) {
			if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
			auto ibn = (IMAGE_IMPORT_BY_NAME*)(base + names->u1.AddressOfData);
			if (strcmp((char*)ibn->Name, "TrackPopupMenu") != 0) continue;
			DWORD old;
			if (!VirtualProtect(&funcs->u1.Function, sizeof(void*), PAGE_READWRITE, &old)) return false;
			// 他のプラグインが先に差し替えていれば、それを呼ぶ
			g_origTrack = (TrackPopupMenu_t)funcs->u1.Function;
			g_iatSlot = (ULONG_PTR*)&funcs->u1.Function;
			funcs->u1.Function = (ULONG_PTR)hook_TrackPopupMenu;
			VirtualProtect(&funcs->u1.Function, sizeof(void*), old, &old);
			return true;
		}
	}
	return false;
}

//---------------------------------------------------------------------
//  プロジェクトへの保存（シーンごと）
//---------------------------------------------------------------------
static void project_load(PROJECT_FILE* pf) {
	g_pins.clear();
	LPCSTR s = pf->get_param_string("pins");
	if (s) {
		int scene, layer, n = 0;
		while (sscanf(s, "%d:%d%n", &scene, &layer, &n) == 2) {
			g_pins[scene].insert(layer);
			s += n;
			if (*s != ',') break;
			s++;
		}
	}
	g_geoTime = 0;
	dbg(L"project_load pins=%d", (int)g_pins.size());
}
static void project_save(PROJECT_FILE* pf) {
	std::string s;
	// シーンID:レイヤー を並べる（同じシーンが複数回出てよい）
	for (auto& [scene, layers] : g_pins)
		for (int layer : layers) {
			if (!s.empty()) s += ",";
			s += std::to_string(scene) + ":" + std::to_string(layer);
		}
	if (s.empty()) pf->clear_params();
	else pf->set_param_string("pins", s.c_str());
}

//---------------------------------------------------------------------
//  公開関数
//---------------------------------------------------------------------
static COMMON_PLUGIN_TABLE g_table = {
	L"レイヤー固定",
	L"レイヤー固定 " VERSION_TEXT L" by arcana",
};

EXTERN_C __declspec(dllexport) COMMON_PLUGIN_TABLE* GetCommonPluginTable(void) { return &g_table; }
EXTERN_C __declspec(dllexport) void InitializeLogger(LOG_HANDLE* h) { g_log = h; }
EXTERN_C __declspec(dllexport) bool InitializePlugin(DWORD) { return true; }
EXTERN_C __declspec(dllexport) void UninitializePlugin() {
	if (g_iatSlot && *g_iatSlot == (ULONG_PTR)hook_TrackPopupMenu) {
		DWORD old;
		VirtualProtect(g_iatSlot, sizeof(void*), PAGE_READWRITE, &old);
		*g_iatSlot = (ULONG_PTR)g_origTrack;
		VirtualProtect(g_iatSlot, sizeof(void*), old, &old);
	}
	if (g_ov) { KillTimer(g_ov, 1); DestroyWindow(g_ov); g_ov = nullptr; }
}

EXTERN_C __declspec(dllexport) void RegisterPlugin(HOST_APP_TABLE* host) {
	HMODULE self = nullptr;
	GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&RegisterPlugin, &self);
	g_inst = self;
	wchar_t path[MAX_PATH];
	GetModuleFileNameW(self, path, MAX_PATH);
	std::wstring d(path);
	d = d.substr(0, d.find_last_of(L'\\'));   // Plugin
	g_dataDir = d.substr(0, d.find_last_of(L'\\'));
	g_test = GetEnvironmentVariableW(L"LAYER_PIN_TEST", nullptr, 0) > 0;
	load_style();

	host->set_plugin_information(g_table.information);
	g_edit = host->create_edit_handle();
	// レイヤー名の欄の右クリックメニューに足す。差し替えられなかったときだけ「プラグイン」メニューに置く
	if (!hook_iat()) {
		host->register_layer_menu(L"このレイヤーを一番上に固定(切り替え)", menu_toggle);
	}
	host->register_project_load_handler(project_load);
	host->register_project_save_handler(project_save);

	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpszClassName = L"LayerPinOverlay";
	wc.lpfnWndProc = ov_proc;
	wc.hInstance = self;
	wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
	RegisterClassExW(&wc);
	// 親（本体の窓）はまだ取れないので、最初の tick で子ウィンドウにする
	WNDCLASSEXW bc{sizeof(bc)};
	bc.lpszClassName = L"LayerPinFrame";
	bc.lpfnWndProc = DefWindowProcW;
	bc.hInstance = self;
	bc.hbrBackground = CreateSolidBrush(color(L"BorderFocus", RGB(0x80, 0x80, 0xe0)).c1);
	RegisterClassExW(&bc);
	g_ov = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, self, nullptr);
	SetTimer(g_ov, 1, 33, nullptr);
}
