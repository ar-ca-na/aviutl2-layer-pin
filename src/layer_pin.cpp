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

#define VERSION_TEXT L"v1.1.0"

static EDIT_HANDLE* g_edit = nullptr;
static LOG_HANDLE* g_log = nullptr;
static HWND g_host = nullptr;
static HWND g_ov = nullptr;
static HWND g_snap = nullptr;   // 押す直前の画面の写しを見せる窓
static HWND g_snapBar = nullptr;   // 同じく縦スクロールバーの上の部分
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
static bool g_ready = false;       // プロジェクトのロード通知（起動時の初期化を含む）が来て、本体の API を呼べるようになったか
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
static int g_sectionSize = 4;   // 中間点の▼の高さ（オブジェクトの上の隙間に描かれる）

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
	f = g_style.find(L"Layout.LayerSectionSize");
	if (f != g_style.end()) g_sectionSize = _wtoi(f->second.c_str());
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

// 重ねた行の操作は本体に任せる（操作の最中＝Op）。
// 押された固定行に、その固定レイヤーの本来の行が来るよう本体を一時的に縦スクロールし、
// その行だけ重ねた窓を外して本体に見せる。それより下は押す直前の画面の写しで覆うので、見た目はずれない。
// 本体のドラッグ中にカーソルが固定行へ入ったときも同じようにスクロールし、出たら元の位置へ戻す
// （本体はドラッグ中も、今の表示位置とカーソルの位置から落とす先を決める）。
static const UINT WM_LP_END = WM_APP + 0x31;
static struct Op {
	bool active = false;
	bool button = false;   // 本体が左ボタンのドラッグを続けている
	bool right = false;    // 右クリックのメニュー待ち
	bool menu = false;     // メニューが出た
	int S = 0;             // 元の表示開始レイヤー
	int row = -1;          // 本体を見せている固定行（-1 なら元の位置）
	int setTo = 0;         // そのために合わせた表示開始レイヤー
	std::vector<int> layers;  // 固定行のレイヤー（開始時）
	DWORD time = 0;
} g_op;

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
	std::vector<int> sections;  // 中間点（2 番目以降の区間の開始フレーム）
	bool operator==(const Obj& o) const {
		return h == o.h && start == o.start && end == o.end && name == o.name && kind == o.kind && sel == o.sel && focus == o.focus &&
			sections == o.sections;
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
	int curLayer = -1;      // 選択中のレイヤー
	int rangeStart = -1, rangeEnd = -1;
	RECT rc{};  // 重ねる窓の位置（ホストのクライアント座標）
	bool operator==(const View& o) const {
		return show == o.show && rows == o.rows && cursor == o.cursor && curLayer == o.curLayer && rangeStart == o.rangeStart &&
			rangeEnd == o.rangeEnd && EqualRect(&rc, &o.rc);
	}
};
static View g_view;

// マウスが乗っている所（本体と同じく、行・オブジェクトの色とカーソルの形を変える）
static struct Hover {
	int row = -1;
	OBJECT_HANDLE obj = nullptr;
	bool edge = false;   // オブジェクトの端（長さを変えられる）
	bool header = false; // レイヤー名の欄（本体は行を明るくする）
	bool operator==(const Hover& o) const { return row == o.row && obj == o.obj && edge == o.edge && header == o.header; }
} g_hover;
static const int EDGE_GRIP = 8;   // 本体は端の ±8px でカーソルを ↔ にする（実測）

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
		if (f >= frames.size()) { SetWindowPos(g_bars[i], nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW); continue; }
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
			o.sel = false;   // 複数選択の一覧に入っているものだけ明るく塗る（フォーカスは点線の枠）
			for (auto s : sel) if (s == h) o.sel = true;
			int ns = edit->get_object_section_num(h);
			for (int s = 1; s < ns; s++) {
				int sf = edit->get_object_section_frame(h, s);
				if (sf > o.start && sf <= o.end) o.sections.push_back(sf);
			}
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
		} else if (line == L"dumpsel") {
			g_edit->call_read_section_param(nullptr, [](void*, EDIT_SECTION* e) {
				OBJECT_HANDLE f = e->get_focus_object();
				if (f) { auto lf = e->get_object_layer_frame(f); dbg(L"focus L%d %d..%d", lf.layer, lf.start, lf.end); }
				int n = e->get_selected_object_num();
				for (int i = 0; i < n; i++) { auto lf = e->get_object_layer_frame(e->get_selected_object(i)); dbg(L"sel L%d %d..%d", lf.layer, lf.start, lf.end); }
			});
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
static WNDPROC g_hostProc = nullptr;
static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM w, LPARAM l);
static void op_end();
static void op_refresh();

static void tick() {
	// 起動処理中（プロジェクトのロード通知の前）に本体の API を呼ぶと落ちる
	// （新しいスクリプトの「追加」確認が出ている間に enum_effect_name で落ちた）。
	// モーダルなダイアログが出ている間（本体の窓が無効）も、本体の状態が途中のことがあるので触らない
	if (!g_ready) return;
	if (g_host && !IsWindowEnabled(g_host)) return;
	if (!g_host) {
		g_host = g_edit->get_host_app_window();
		if (!g_host) return;
		SetWindowLongPtrW(g_ov, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS);
		SetParent(g_ov, g_host);
		dbg(L"host=%p style=%08lx", g_host, (unsigned long)GetWindowLongPtrW(g_host, GWL_STYLE));
		// 押す直前の画面の写し（クリックは本体へ通す）
		g_snap = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinSnapshot", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, 0, 0, 0, 0, g_host, nullptr, g_inst, nullptr);
		g_snapBar = CreateWindowExW(WS_EX_NOACTIVATE, L"LayerPinSnapshot", L"", WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED, 0, 0, 0, 0, g_host, nullptr, g_inst, nullptr);
		// 本体のドラッグ中、カーソルが固定行に入ったかを見るため
		g_hostProc = (WNDPROC)SetWindowLongPtrW(g_host, GWLP_WNDPROC, (LONG_PTR)host_proc);
	}
	if (!g_effectTypesLoaded) {
		g_effectTypesLoaded = true;
		g_edit->enum_effect_name(nullptr, [](void*, LPCWSTR name, int type, int flag) { g_effectTypes[name] = {type, flag}; });
	}
	if (g_op.active) {
		// 右クリックを渡したのにメニューが出なかったとき・ボタンの離しを取りこぼしたときは、少ししたら戻す
		bool l = GetAsyncKeyState(VK_LBUTTON) & 0x8000, r = GetAsyncKeyState(VK_RBUTTON) & 0x8000;
		if (g_op.right ? (!g_op.menu && !r && GetTickCount() - g_op.time > 800) : (!l && GetTickCount() - g_op.time > 800)) op_end();
		else { op_refresh(); return; }
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
		if (g_edit->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) {
			// 再生中に編集セクションを呼ぶと再生が止まる。横スクロールだけは前回の位置から計算で追う
			GeoKey s = g_geoKey; s.frame_start = k.frame_start;
			if (g_geo.ok && s == k && k.frame_start != g_geoKey.frame_start) {
				g_geo.f0 += k.frame_start - g_geoKey.frame_start;
				g_geoKey = k;
			}
		} else if (!(k == g_geoKey) || now - g_geoTime > 1000) {
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
		v.curLayer = info.layer;
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
		if (IsWindowVisible(g_ov)) SetWindowPos(g_ov, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
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

// 今ある絵の上に、色 c を不透明度 alpha で重ねる
static void fill_alpha(HDC dc, RECT r, COLORREF c, BYTE alpha) {
	if (r.right <= r.left || r.bottom <= r.top) return;
	BLENDFUNCTION bf{AC_SRC_OVER, 0, alpha, 0};
	HDC tdc = CreateCompatibleDC(dc);
	HBITMAP tb = CreateCompatibleBitmap(dc, 1, 1);
	HGDIOBJ ob = SelectObject(tdc, tb);
	SetPixel(tdc, 0, 0, c);
	AlphaBlend(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, tdc, 0, 0, 1, 1, bf);
	SelectObject(tdc, ob); DeleteObject(tb); DeleteDC(tdc);
}

static COLORREF blend(COLORREF a, COLORREF b, int alpha) {
	return RGB((GetRValue(a) * (255 - alpha) + GetRValue(b) * alpha) / 255,
		(GetGValue(a) * (255 - alpha) + GetGValue(b) * alpha) / 255,
		(GetBValue(a) * (255 - alpha) + GetBValue(b) * alpha) / 255);
}

// フォーカス中のオブジェクトの枠：本体は外周 1px の点線（約 7.5px 周期、半分が線。端はぼかす）。左上から時計回り
static void dashed_frame(HDC dc, RECT r, COLORREF c) {
	const double period = 7.5, on = 3.75;
	auto cover = [&](double s) {   // [s, s+1) のうち線にかかる割合
		double k = floor(s / period) * period, sum = 0;
		for (double a = k - period; a < s + 1; a += period) {
			double lo = (std::max)(a, s), hi = (std::min)(a + on, s + 1);
			if (hi > lo) sum += hi - lo;
		}
		return sum;
	};
	auto put = [&](int x, int y, double s) {
		double cv = cover(s);
		if (cv <= 0) return;
		SetPixel(dc, x, y, blend(GetPixel(dc, x, y), c, (int)(cv * 255 + 0.5)));
	};
	int w = r.right - 1 - r.left, h = r.bottom - 1 - r.top;
	if (w <= 0 || h <= 0) return;
	for (int x = r.left; x < r.right; x++) put(x, r.top, x - r.left);
	for (int y = r.top + 1; y < r.bottom; y++) put(r.right - 1, y, w + (y - r.top));
	for (int x = r.right - 2; x >= r.left; x--) put(x, r.bottom - 1, w + h + (r.right - 1 - x));
	for (int y = r.bottom - 2; y > r.top; y--) put(r.left, y, 2 * w + h + (r.bottom - 1 - y));
}

// 本体のオブジェクトは左から右へのグラデーション
static void hgrad(HDC dc, RECT r, COLORREF left, COLORREF right) {
	TRIVERTEX v[2] = {
		{r.left, r.top, (COLOR16)(GetRValue(left) << 8), (COLOR16)(GetGValue(left) << 8), (COLOR16)(GetBValue(left) << 8), 0},
		{r.right, r.bottom, (COLOR16)(GetRValue(right) << 8), (COLOR16)(GetGValue(right) << 8), (COLOR16)(GetBValue(right) << 8), 0},
	};
	GRADIENT_RECT gr{0, 1};
	GradientFill(dc, v, 2, &gr, 1, GRADIENT_FILL_RECT_H);
}

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
	Col cHoverObj = color(L"ObjectHover", RGB(0xff, 0xff, 0xff));
	Col cLayerHover = color(L"LayerHover", RGB(0x58, 0x58, 0x58));
	Col cLayerSel = color(L"LayerSelect", RGB(0x3c, 0x40, 0x54));
	Col cCursor = color(L"FrameCursor", RGB(0xc8, 0x30, 0x30));
	Col cCursorWide = color(L"FrameCursorWide", RGB(0xc8, 0x30, 0x30));
	Col cRange = color(L"LayerRange", RGB(0x70, 0x70, 0x70));
	Col cPin = color(L"BorderFocus", RGB(0x80, 0x80, 0xe0));
	Col cBorder = color(L"Border", RGB(0x90, 0x90, 0x90));
	Col cFocusDash = color(L"BorderSelect", RGB(0xe0, 0xe0, 0xe0));
	const View& v = g_view;
	static const wchar_t* kinds[][2] = {
		{L"ObjectVideo", L"ObjectVideoSelect"}, {L"ObjectAudio", L"ObjectAudioSelect"},
		{L"ObjectControl", L"ObjectControlSelect"}, {L"ObjectVideoFilter", L"ObjectVideoFilterSelect"},
		{L"ObjectAudioFilter", L"ObjectAudioFilterSelect"}, {L"ObjectVideoEffect", L"ObjectVideoEffectSelect"},
	};
	Col cSection = color(L"ObjectSection", RGB(0xa0, 0xa0, 0xa0));
	// 本体と同じく、行の上端の線 1px と中間点の▼の隙間を空けて、その下に描く
	const int rh = g_geo.rowH;
	const int objOff = (std::min)(g_sectionSize + 2, rh / 2);
	auto draw_obj = [&](const Obj& o, RECT r, RECT clip, bool enable, bool selected) {
		int saved = SaveDC(dc);
		IntersectClipRect(dc, clip.left, clip.top, clip.right, clip.bottom);
		Col base = color(kinds[o.kind][0], RGB(0x30, 0x40, 0xe0));
		if (selected) { Col sc = color(kinds[o.kind][1], base.c1); hgrad(dc, r, sc.c1, sc.c1); }
		else hgrad(dc, r, base.c1, base.c2);
		if (!enable) {
			// 非表示レイヤーは暗く
			BLENDFUNCTION bf{AC_SRC_OVER, 0, 128, 0};
			HDC tdc = CreateCompatibleDC(dc); HBITMAP tb = CreateCompatibleBitmap(dc, 1, 1); HGDIOBJ ob = SelectObject(tdc, tb);
			SetPixel(tdc, 0, 0, cLayerDis.c1);
			AlphaBlend(dc, clip.left, clip.top, clip.right - clip.left, clip.bottom - clip.top, tdc, 0, 0, 1, 1, bf);
			SelectObject(tdc, ob); DeleteObject(tb); DeleteDC(tdc);
		}
		if (o.h == g_hover.obj) {
			auto ring = [&](RECT q, BYTE a) {
				fill_alpha(dc, {q.left, q.top, q.right, q.top + 1}, cHoverObj.c1, a);
				fill_alpha(dc, {q.left, q.bottom - 1, q.right, q.bottom}, cHoverObj.c1, a);
				fill_alpha(dc, {q.left, q.top + 1, q.left + 1, q.bottom - 1}, cHoverObj.c1, a);
				fill_alpha(dc, {q.right - 1, q.top + 1, q.right, q.bottom - 1}, cHoverObj.c1, a);
			};
			RestoreDC(dc, saved); saved = SaveDC(dc);   // 外側の 1px はオブジェクトの外に描く
			IntersectClipRect(dc, clip.left - 1, clip.top - 1, clip.right + 1, clip.bottom + 1);
			RECT out = r; InflateRect(&out, 1, 1);
			ring(out, (BYTE)(cHoverObj.a * 50 / 100));
			ring(r, cHoverObj.a);
			RECT in = r; InflateRect(&in, -1, -1);
			ring(in, (BYTE)(cHoverObj.a * 50 / 100));
		}
		if (o.focus) dashed_frame(dc, r, cFocusDash.c1);
		RECT tr = clip;
		tr.left = (r.left < areaL ? areaL : r.left) + 4;
		tr.right -= 2;
		SetTextColor(dc, enable ? cText.c1 : cTextDis.c1);
		DrawTextW(dc, o.name.c_str(), -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
		RestoreDC(dc, saved);
	};
	// 中間点：オブジェクトの上の隙間に▼（本体と同じ形：幅 2S+1 から 2 ずつ細る S 段）
	auto draw_sections = [&](const Obj& o, int top, int dx, RECT body) {
		for (int sf : o.sections) {
			int cx = (int)floor(frame_to_x(sf + 0.5)) - org.x + dx;  // 本体はコマの中ほどに描く
			for (int k = 0; k < g_sectionSize; k++) {
				int hw = g_sectionSize - k;
				RECT r{cx - hw, top + 2 + k, cx + hw + 1, top + 3 + k}, c;
				if (IntersectRect(&c, &r, &body)) fill(dc, c, cSection.c1);
			}
		}
	};

	for (size_t i = 0; i < v.rows.size(); i++) {
		const Row& row = v.rows[i];
		int top = (int)i * rh, bottom = top + rh;
		// 背景
		RECT body{areaL, top, rc.right, bottom};
		COLORREF bg = !row.layerEnable ? cLayerDis.c1 : (g_hover.header && g_hover.row == (int)i) ? cLayerHover.c1 : row.layer == v.curLayer ? cLayerSel.c1 : cLayer.c1;
		fill(dc, body, bg);
		if (v.rangeStart >= 0 && v.rangeEnd >= v.rangeStart) {
			RECT r{(int)floor(frame_to_x(v.rangeStart)) - org.x, top, (int)floor(frame_to_x(v.rangeEnd + 1)) - org.x, bottom};
			IntersectRect(&r, &r, &body);
			fill(dc, r, blend(bg, cRange.c1, cRange.a));
		}
		// オブジェクト（ドラッグ中のものは後で上に描く）
		for (auto& o : row.objs) {
			RECT r{obj_left(org.x, o.start), top + objOff, obj_left(org.x, o.end + 1), bottom};
			RECT clip;
			if (IntersectRect(&clip, &r, &body)) draw_obj(o, r, clip, row.layerEnable, o.sel);
			draw_sections(o, top, 0, body);
		}
		// 現在フレームのカーソル
		{
			int cx0 = (int)floor(frame_to_x(v.cursor)) - org.x, cx1 = (int)floor(frame_to_x(v.cursor + 1)) - org.x;
			bool wide = g_geo.ppf > 1.0;   // 本体は 1 フレームが 1px より広いと帯、それ以下は細い線（実測）
			RECT cr = wide ? RECT{cx0, top, cx1, bottom} : RECT{cx0, top, cx0 + 1, bottom};
			Col cc = wide ? cCursorWide : cCursor;
			IntersectRect(&cr, &cr, &body);
			fill_alpha(dc, cr, cc.c1, cc.a);
		}
		// レイヤー名の欄
		if (areaL > 0) {
			fill(dc, RECT{0, top, areaL, bottom}, (g_hover.header && g_hover.row == (int)i) ? cLayerHover.c1 : row.layer == v.curLayer ? cLayerSel.c1 : cHeader.c1);
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
		// 行の境目（本体と同じく行の上端）
		fill(dc, RECT{areaL, top, rc.right, top + 1}, cBorder.c1);
	}
	// 下端の線：固定の行だと分かるように（操作の最中に上の行だけを見せているときは引かない）
	if (!g_op.active || g_op.row < 0) fill(dc, RECT{0, rc.bottom - 2, rc.right, rc.bottom}, cPin.c1);

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

static void pins_changed() {
	g_geoTime = 0;
	if (g_ov) PostMessageW(g_ov, WM_TIMER, 1, 0);
}

//---------------------------------------------------------------------
//  操作の最中（Op）：固定行の操作を本体に任せる
//---------------------------------------------------------------------
static HDC g_snapDC = nullptr;
static HBITMAP g_snapBmp = nullptr;
static HGDIOBJ g_snapOld = nullptr;
static RECT g_snapRect{};              // 写した範囲（スクリーン座標）

static int dls_now() {
	EDIT_INFO info{};
	g_edit->get_edit_info(&info, sizeof(info));
	return info.display_layer_start;
}

static void free_snapshot() {
	if (!g_snapDC) return;
	SelectObject(g_snapDC, g_snapOld);
	DeleteObject(g_snapBmp);
	DeleteDC(g_snapDC);
	g_snapDC = nullptr; g_snapBmp = nullptr; g_snapOld = nullptr;
}

// レイヤー編集の上端から下端まで（縦スクロールバーも含む）を、画面から写し取る
static void take_snapshot() {
	free_snapshot();
	g_snapRect = {g_geo.left, g_geo.top, g_geo.right + g_scrollBar, g_geo.bottom};
	int w = g_snapRect.right - g_snapRect.left, h = g_snapRect.bottom - g_snapRect.top;
	if (w <= 0 || h <= 0) return;
	HDC sdc = GetDC(nullptr);
	g_snapDC = CreateCompatibleDC(sdc);
	g_snapBmp = CreateCompatibleBitmap(sdc, w, h);
	g_snapOld = SelectObject(g_snapDC, g_snapBmp);
	BitBlt(g_snapDC, 0, 0, w, h, sdc, g_snapRect.left, g_snapRect.top, SRCCOPY | CAPTUREBLT);
	ReleaseDC(nullptr, sdc);
}

static void place(HWND w, RECT screen) {
	if (screen.right <= screen.left || screen.bottom <= screen.top) {
		SetWindowPos(w, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
		return;
	}
	POINT a{screen.left, screen.top}, b{screen.right, screen.bottom};
	ScreenToClient(g_host, &a);
	ScreenToClient(g_host, &b);
	SetWindowPos(w, HWND_TOP, a.x, a.y, b.x - a.x, b.y - a.y, SWP_NOACTIVATE | SWP_SHOWWINDOW);
	InvalidateRect(w, nullptr, FALSE);
}

// row 行目だけを本体に見せる：上の固定行は重ねた窓、下は写し。row < 0 なら全部の固定行を重ねる
static void op_layout() {
	update_frames({});
	int r = g_op.row, top = g_geo.rowTop, h = g_geo.rowH;
	if (r < 0) {
		place(g_ov, {g_geo.left, top, g_geo.right, top + h * (int)g_op.layers.size()});
		place(g_snap, {});
		place(g_snapBar, {});
		return;
	}
	place(g_ov, {g_geo.left, top, g_geo.right, top + h * r});
	if (g_snapDC) {
		// 縦スクロールでつまみが動くので、スクロールバーは上から覆う（本体に見せる行の右端も覆うが、そこはスクロールバー）
		place(g_snap, {g_snapRect.left, top + h * (r + 1), g_snapRect.right, g_snapRect.bottom});
		place(g_snapBar, {g_geo.right, g_snapRect.top, g_snapRect.right, top + h * (r + 1)});
	} else {
		place(g_snap, {});
		place(g_snapBar, {});
	}
}

// 本体のドラッグ中に縦スクロールできるのはマウスホイールだけ（編集セクションは使えない）。
// 「マウスホイールで縦スクロール」が切なら Alt+ホイールで縦になる。本体は Alt を GetKeyState で見るので、
// 本体の UI スレッドのキーの状態を一時的に書き換えて送る。どちらで縦に動くかは試して確かめる
static int g_wheelAlt = -1;  // 1: Alt を付ける 0: 付けない -1: 未確認
static void wheel_one(bool alt, bool down) {
	BYTE ks[256], k2[256];
	GetKeyboardState(ks);
	memcpy(k2, ks, sizeof(ks));
	k2[VK_MENU] = k2[VK_LMENU] = k2[VK_RMENU] = alt ? 0x80 : 0;
	k2[VK_CONTROL] = k2[VK_LCONTROL] = k2[VK_RCONTROL] = 0;
	k2[VK_SHIFT] = k2[VK_LSHIFT] = k2[VK_RSHIFT] = 0;
	SetKeyboardState(k2);
	POINT p; GetCursorPos(&p);
	WPARAM btn = (GetKeyState(VK_LBUTTON) & 0x8000) ? MK_LBUTTON : 0;
	SendMessageW(g_host, WM_MOUSEWHEEL, MAKEWPARAM(btn, (WORD)(short)(down ? -WHEEL_DELTA : WHEEL_DELTA)), MAKELPARAM(p.x, p.y));
	SetKeyboardState(ks);
}
static bool wheel_to(int target) {
	bool tried[2] = {false, false};
	for (int guard = 0; guard < 2000; guard++) {
		EDIT_INFO a{}; g_edit->get_edit_info(&a, sizeof(a));
		if (a.display_layer_start == target) return true;
		bool down = target > a.display_layer_start;
		bool alt = g_wheelAlt != 0;
		wheel_one(alt, down);
		EDIT_INFO b{}; g_edit->get_edit_info(&b, sizeof(b));
		if (b.display_layer_start != a.display_layer_start) { g_wheelAlt = alt; tried[0] = tried[1] = false; continue; }
		// 縦に動かなかった。横に動いたなら戻して、Alt の有無を逆にして試す（どちらも動かなければ端に着いた）
		if (b.display_frame_start != a.display_frame_start) wheel_one(alt, !down);
		tried[alt] = true;
		if (tried[!alt]) return false;
		g_wheelAlt = !alt;
	}
	return false;
}

static bool scroll_section(int target) {
	EDIT_INFO info{}; g_edit->get_edit_info(&info, sizeof(info));
	if (info.display_layer_start == target) return true;
	struct P { int a, b; } prm{target, info.display_frame_start};
	return g_edit->call_edit_section_param(&prm, [](void* q, EDIT_SECTION* e) { auto r = (P*)q; e->set_display_layer_frame(r->a, r->b); });
}

static void op_show_row(int r, bool dragging) {
	if (r == g_op.row) return;
	if (g_op.row < 0) {
		g_op.S = dls_now();   // 元の位置（本体がドラッグ中に自動スクロールしていれば、その位置）
		take_snapshot();
	}
	int target = r < 0 ? g_op.S : g_op.layers[r] - r;
	if (dragging || !scroll_section(target)) wheel_to(target);
	g_op.row = r;
	g_op.setTo = dls_now();
	op_layout();
	dbg(L"op row=%d target=%d dls=%d", r, target, dls_now());
}

static bool op_begin() {
	if (!g_view.show || !g_geo.ok || g_edit->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) return false;
	g_op = Op{};
	g_op.active = true;
	g_op.S = dls_now();
	g_op.time = GetTickCount();
	for (auto& r : g_view.rows) g_op.layers.push_back(r.layer);
	return true;
}

static void op_end() {
	if (!g_op.active) return;
	dbg(L"op end row=%d S=%d dls=%d", g_op.row, g_op.S, dls_now());
	if (g_op.row >= 0) {
		// 操作の間に利用者がスクロールした分（右ボタンのドラッグ・ドラッグ中の自動スクロール）は引き継ぐ
		int back = (std::max)(0, g_op.S + dls_now() - g_op.setTo);
		if (!scroll_section(back)) wheel_to(back);
	}
	g_op = Op{};
	free_snapshot();
	place(g_snap, {});
	place(g_snapBar, {});
	g_view = View{};       // 次の tick で置き直す
	SetWindowPos(g_ov, nullptr, 0, 0, 0, 0, SWP_NOZORDER | SWP_NOACTIVATE | SWP_HIDEWINDOW);
	g_geoTime = 0;
	tick();
}

// 操作の最中は並びを変えず、重ねた行の中身だけ読み直す
static void op_refresh() {
	if (g_op.row == 0) return;   // 重ねた行を見せていない
	View v = g_view;
	for (auto& r : v.rows) r.objs.clear();
	EDIT_INFO info{}; g_edit->get_edit_info(&info, sizeof(info));
	v.cursor = info.frame;
	v.curLayer = info.layer;
	v.rangeStart = info.select_range_start;
	v.rangeEnd = info.select_range_end;
	ReadCtx c{v, (int)ceil(x_to_frame(g_geo.right)) + 1};
	g_edit->call_read_section_param(&c, read_objects);
	if (c.v == g_view) return;
	g_view = c.v;
	InvalidateRect(g_ov, nullptr, FALSE);
}

// 本体の窓：ドラッグ中にカーソルが固定行へ入ったら、その固定レイヤーを見せる
static LRESULT CALLBACK host_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_LBUTTONDOWN:
	case WM_LBUTTONDBLCLK:
		if (!g_op.active && g_ready && g_geo.ok) {
			// レイヤーの行の中で押したときだけ（プレビューや時間ゲージのドラッグでは動かさない）
			POINT p; GetCursorPos(&p);
			if (p.x >= g_geo.left && p.x < g_geo.right + g_scrollBar && p.y >= g_geo.rowTop && p.y < g_geo.bottom && op_begin()) dbg(L"op begin (host)");
		}
		if (g_op.active && !g_op.right) { g_op.button = true; g_op.time = GetTickCount(); }
		break;
	case WM_MOUSEMOVE:
		if (g_op.active && g_op.button && (w & MK_LBUTTON)) {
			POINT p; GetCursorPos(&p);
			int n = (int)g_op.layers.size(), r = -1;
			if (p.y >= g_geo.rowTop && p.y < g_geo.rowTop + n * g_geo.rowH && p.x >= g_geo.left && p.x < g_geo.right + g_scrollBar)
				r = (p.y - g_geo.rowTop) / g_geo.rowH;
			op_show_row(r, true);
		}
		break;
	case WM_LBUTTONUP: {
		LRESULT ret = CallWindowProcW(g_hostProc, h, m, w, l);
		if (g_op.active && !g_op.right) { g_op.button = false; PostMessageW(g_ov, WM_LP_END, 0, 0); }
		return ret;
	}
	case WM_RBUTTONDOWN:
	case WM_RBUTTONUP:
	case WM_CONTEXTMENU:
		dbg(L"host msg %04x x=%d y=%d", m, GET_X_LPARAM(l), GET_Y_LPARAM(l));
		break;
	}
	return CallWindowProcW(g_hostProc, h, m, w, l);
}

static LRESULT CALLBACK snap_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
	if (msg == WM_PAINT) {
		PAINTSTRUCT ps;
		HDC dc = BeginPaint(hwnd, &ps);
		RECT rc; GetClientRect(hwnd, &rc);
		POINT o{0, 0}; ClientToScreen(hwnd, &o);
		if (g_snapDC) BitBlt(dc, 0, 0, rc.right, rc.bottom, g_snapDC, o.x - g_snapRect.left, o.y - g_snapRect.top, SRCCOPY);
		EndPaint(hwnd, &ps);
		return 0;
	}
	if (msg == WM_ERASEBKGND) return 1;
	return DefWindowProcW(hwnd, msg, wp, lp);
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
		SetCursor(LoadCursorW(nullptr, g_hover.edge ? IDC_SIZEWE : IDC_ARROW));
		return TRUE;
	case WM_MOUSEMOVE: {
		Hover hv;
		int x = GET_X_LPARAM(lp);
		hv.row = hit_row(GET_Y_LPARAM(lp));
		hv.header = hv.row >= 0 && x < g_headerWidth;
		if (hv.row >= 0 && x >= g_headerWidth) {
			POINT org{0, 0}; ClientToScreen(hwnd, &org);
			for (auto& o : g_view.rows[hv.row].objs) {
				int L = obj_left(org.x, o.start), R = obj_left(org.x, o.end + 1);
				if ((x >= L - EDGE_GRIP && x <= L + EDGE_GRIP) || (x >= R - EDGE_GRIP && x < R + EDGE_GRIP)) hv.edge = true;
				if (x >= L && x < R) hv.obj = o.h;
			}
		}
		if (!(hv == g_hover)) {
			g_hover = hv;
			InvalidateRect(hwnd, nullptr, FALSE);
			SetCursor(LoadCursorW(nullptr, hv.edge ? IDC_SIZEWE : IDC_ARROW));
		}
		TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, hwnd, 0};
		TrackMouseEvent(&t);
		return 0;
	}
	case WM_MOUSELEAVE:
		if (!(g_hover == Hover{})) { g_hover = Hover{}; InvalidateRect(hwnd, nullptr, FALSE); }
		return 0;
	case WM_MOUSEACTIVATE:
		return MA_NOACTIVATE;
	case WM_LBUTTONDOWN:
	case WM_RBUTTONDOWN: {
		// 押された固定行を本体に見せ、押す操作を本体へ渡す（後の動き・離す操作は本体に直接届く）
		int row = hit_row(GET_Y_LPARAM(lp));
		if (row < 0 || g_op.active) return 0;
		POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
		MapWindowPoints(hwnd, g_host, &p, 1);   // 窓を動かす前に本体の座標へ直す
		bool header = GET_X_LPARAM(lp) < g_headerWidth;
		if (!op_begin()) {
			// 再生中・出力中は編集セクションを呼べない（呼ぶと再生が止まる）。本体は再生中のクリックで再生を止めるだけなので、
			// そのまま渡す（離す操作は、本体が押下で掴むので本体に届く）
			if (g_edit->get_edit_state() != EDIT_HANDLE::EDIT_STATE_EDIT) {
				PostMessageW(g_host, WM_MOUSEMOVE, wp & ~(MK_LBUTTON | MK_RBUTTON), MAKELPARAM(p.x, p.y));
				PostMessageW(g_host, msg, wp, MAKELPARAM(p.x, p.y));
			}
			return 0;
		}
		g_op.right = msg == WM_RBUTTONDOWN;
		op_show_row(row, false);
		dbg(L"press %04x row=%d layer=%d header=%d", msg, row, g_op.layers[row], header);
		// レイヤー名の欄の右クリックは、離したときの WM_CONTEXTMENU でメニューが出る。押下を渡すと出なくなる。
		// 本体はその位置に子ウィンドウがあると出さないので、押された行には窓を置かない（op_layout）
		if (g_op.right && header) return 0;
		PostMessageW(g_host, WM_MOUSEMOVE, wp & ~(MK_LBUTTON | MK_RBUTTON), MAKELPARAM(p.x, p.y));
		PostMessageW(g_host, msg, wp, MAKELPARAM(p.x, p.y));
		return 0;
	}
	case WM_LP_END:
		op_end();
		return 0;
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

static BOOL WINAPI hook_inner(HMENU m, UINT flags, int x, int y, int r, HWND h, const RECT* rc);
static BOOL WINAPI hook_TrackPopupMenu(HMENU m, UINT flags, int x, int y, int r, HWND h, const RECT* rc) {
	bool fwd = g_op.active && g_op.right;
	if (fwd) g_op.menu = true;
	BOOL ret = hook_inner(m, flags, x, y, r, h, rc);
	// 選んだ項目の処理が済んでから縦位置を戻す
	if (fwd && g_ov) PostMessageW(g_ov, WM_LP_END, 0, 0);
	return ret;
}

static BOOL WINAPI hook_inner(HMENU m, UINT flags, int x, int y, int r, HWND h, const RECT* rc) {
	if (!g_edit || !g_ready || !menu_has(m, L"レイヤー名を変更")) return g_origTrack(m, flags, x, y, r, h, rc);
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
	g_ready = true;
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
	g_ready = false;
	if (g_iatSlot && *g_iatSlot == (ULONG_PTR)hook_TrackPopupMenu) {
		DWORD old;
		VirtualProtect(g_iatSlot, sizeof(void*), PAGE_READWRITE, &old);
		*g_iatSlot = (ULONG_PTR)g_origTrack;
		VirtualProtect(g_iatSlot, sizeof(void*), old, &old);
	}
	if (g_host && g_hostProc && GetWindowLongPtrW(g_host, GWLP_WNDPROC) == (LONG_PTR)host_proc)
		SetWindowLongPtrW(g_host, GWLP_WNDPROC, (LONG_PTR)g_hostProc);
	if (g_ov) { KillTimer(g_ov, 1); DestroyWindow(g_ov); g_ov = nullptr; }
	if (g_snap) { DestroyWindow(g_snap); g_snap = nullptr; }
	if (g_snapBar) { DestroyWindow(g_snapBar); g_snapBar = nullptr; }
	free_snapshot();
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
	// 本体の窓クラスは CS_DBLCLKS を持たない（ダブルクリックも押下 2 回で届く）ので、こちらも持たない
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
	WNDCLASSEXW sc{sizeof(sc)};
	sc.lpszClassName = L"LayerPinSnapshot";
	sc.lpfnWndProc = snap_proc;
	sc.hInstance = self;
	RegisterClassExW(&sc);
	g_ov = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, self, nullptr);
	SetTimer(g_ov, 1, 33, nullptr);
}
