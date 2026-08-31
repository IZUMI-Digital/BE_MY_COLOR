// ============================================================================
// BE MY COLOR - Color Recognition Utility
// マウスカーソル周辺の色を取得し、代表色を日本語名とともに表示する簡易ツール。
// Win32 API を用いた単一ファイル構成の GUI アプリケーション。
//
// Copyright 2026 IZUMI-Digital
// Licensed under the MIT License
// ============================================================================

#define NOMINMAX

#include <windows.h>
#include <string>
#include <vector>
#include <cmath>
#include <sstream>
#include <iomanip>
#include <algorithm>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")

// ============================================================================
// アプリ設定と定数
// ============================================================================

constexpr int SAMPLE_N = 7;                    // サンプル領域の一辺（ピクセル）
constexpr bool ENABLE_DPI_AWARENESS = true;    // DPI 対応を有効にするか
constexpr UINT TIMER_INTERVAL_MS = 100;        // 色取得のタイマー間隔（ms）

static constexpr wchar_t NO_COLOR_STR[] = L"—"; // 色が無効なときの表示

constexpr int BUTTON_ID_TOPMOST  = 3001;
constexpr int BUTTON_ID_BOTTOM   = 3002;
constexpr int BUTTON_ID_MINIMIZE = 3003;
constexpr int BUTTON_ID_EXIT     = 3004;

constexpr double DELTA_E_THRESHOLD = 6.0;   // 初期クラスタリング閾値
constexpr double MERGE_DELTA_E     = 5.0;   // クラスタ統合閾値
constexpr double MIN_GROUP_SHARE   = 0.10;  // 表示対象とする最小割合
constexpr int    TOP_K_CLUSTERS    = 10;    // 上位クラスタの最大数

const int WINDOW_PADDING = 8;
const int SWATCH_SIZE    = 48;
const int WINDOW_WIDTH   = 420;
const int WINDOW_HEIGHT  = 140;

constexpr double PI_CONST = 3.14159265358979323846;

// ============================================================================
// 前方宣言
// ============================================================================

LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
void EnableDpiAwarenessIfRequested();
bool GetBlockViaBitBlt(int x, int y, int w, int h, std::vector<COLORREF>& outPixels);
std::pair<std::pair<COLORREF, std::wstring>, std::pair<COLORREF, std::wstring>>
    GetTopTwoMergedClusters(const std::vector<COLORREF>& pixels);
std::wstring MakePracticalJapaneseName(COLORREF c);
std::wstring ColorToHex(COLORREF c);

// 色変換用構造体
struct Xyz { double x; double y; double z; };
struct Lab { double L; double a; double b; };
struct Hsl { double h; double s; double l; };

// 色変換関数
Xyz RgbToXyz(int r, int g, int b);
Lab XyzToLab(const Xyz& xyz);
Lab RgbToLab(int r, int g, int b);
Hsl RgbToHsl(int r, int g, int b);
double DeltaE2000(const Lab& c1, const Lab& c2);

inline double Deg2Rad(double deg) {
    return deg * PI_CONST / 180.0;
}

inline double Rad2Deg(double rad) {
    return rad * 180.0 / PI_CONST;
}

// 描画共通関数
void DrawColorCluster(HDC hdc, HFONT hFontTitle, HFONT hFontSmall, int swX, int swY, COLORREF color, const std::wstring& name);

// ============================================================================
// アプリケーション状態（グローバル）
// ============================================================================

struct AppState {
    bool      dragging   = false;
    POINT     dragOffset { 0, 0 };
    bool      isTopMost  = true;
    COLORREF  lastColor1 = RGB(0, 0, 0);   // 起動時に黒を表示して UI を安定させる
    COLORREF  lastColor2 = CLR_INVALID;   // 2位は存在しない可能性があるため無効で初期化
    std::wstring lastName1 = NO_COLOR_STR;
    std::wstring lastName2 = NO_COLOR_STR;
} g_state;

// ============================================================================
// ヘルパー: HEX 表示
// ============================================================================

std::wstring ColorToHex(COLORREF c) {
    std::wstringstream ss;
    ss << L"#" << std::uppercase << std::hex
       << std::setw(2) << std::setfill(L'0') << (int)GetRValue(c)
       << std::setw(2) << std::setfill(L'0') << (int)GetGValue(c)
       << std::setw(2) << std::setfill(L'0') << (int)GetBValue(c);
    return ss.str();
}

// ============================================================================
// DPI 対応設定（簡易）
// ============================================================================

void EnableDpiAwarenessIfRequested() {
    if (!ENABLE_DPI_AWARENESS) {
        return;
    }

    HMODULE user32 = LoadLibraryW(L"user32.dll");
    if (user32) {
        typedef BOOL(WINAPI* SPDAC)(DPI_AWARENESS_CONTEXT);
        SPDAC p = (SPDAC)GetProcAddress(user32, "SetProcessDpiAwarenessContext");
        if (p) {
            p(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
        FreeLibrary(user32);
    }
}

// ============================================================================
// 画面キャプチャ（BitBlt）
// 説明:
//   指定矩形をスクリーンから取得し、COLORREF の配列を返す。
//   呼び出し側は outPixels のサイズを気にする必要はない。
// ============================================================================

bool GetBlockViaBitBlt(int x, int y, int w, int h, std::vector<COLORREF>& outPixels) {
    outPixels.clear();

    if (w <= 0 || h <= 0) {
        return false;
    }

    outPixels.resize(w * h, RGB(0, 0, 0));

    HDC hdcScreen = GetDC(nullptr);
    if (!hdcScreen) {
        return false;
    }

    HDC hdcMem = CreateCompatibleDC(hdcScreen);
    if (!hdcMem) {
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    HBITMAP hBmp = CreateCompatibleBitmap(hdcScreen, w, h);
    if (!hBmp) {
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    HBITMAP hOld = (HBITMAP)SelectObject(hdcMem, hBmp);

    if (!BitBlt(hdcMem, 0, 0, w, h, hdcScreen, x, y, SRCCOPY)) {
        SelectObject(hdcMem, hOld);
        DeleteObject(hBmp);
        DeleteDC(hdcMem);
        ReleaseDC(nullptr, hdcScreen);
        return false;
    }

    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            outPixels[yy * w + xx] = GetPixel(hdcMem, xx, yy);
        }
    }

    SelectObject(hdcMem, hOld);
    DeleteObject(hBmp);
    DeleteDC(hdcMem);
    ReleaseDC(nullptr, hdcScreen);
    return true;
}

// ============================================================================
// RGB -> HSL（構造体で返す）
// 説明:
//   参照渡しを廃止し、戻り値で Hsl を返す（RVO による効率化を期待）
// ============================================================================

Hsl RgbToHsl(int r, int g, int b) {
    double R = r / 255.0;
    double G = g / 255.0;
    double B = b / 255.0;

    double maxv = std::max(R, std::max(G, B));
    double minv = std::min(R, std::min(G, B));
    double l = (maxv + minv) / 2.0;
    double h = 0.0;
    double s = 0.0;

    if (maxv == minv) {
        return Hsl{ 0.0, 0.0, l };
    }

    double d = maxv - minv;
    s = (l > 0.5) ? d / (2.0 - maxv - minv) : d / (maxv + minv);

    if (maxv == R) {
        h = (G - B) / d + (G < B ? 6.0 : 0.0);
    } else if (maxv == G) {
        h = (B - R) / d + 2.0;
    } else {
        h = (R - G) / d + 4.0;
    }

    h *= 60.0;
    return Hsl{ h, s, l };
}

// ============================================================================
// 日本語色名の簡易生成
// 説明:
//   HSL を用いて大まかな日本語色名を返す。第三者が読みやすいように
//   意図（無彩色判定、色相区分、修飾語）をコメントで補足している。
// ============================================================================

std::wstring MakePracticalJapaneseName(COLORREF c) {
    if (c == CLR_INVALID) {
        return NO_COLOR_STR;
    }

    int r = GetRValue(c);
    int g = GetGValue(c);
    int b = GetBValue(c);

    Hsl hsl = RgbToHsl(r, g, b);
    double h = hsl.h;
    double s = hsl.s;
    double l = hsl.l;

    // 無彩色の判定（彩度が非常に低い場合）
    if (s < 0.05) {
        if (l > 0.92) {
            return L"白";
        }
        if (l < 0.08) {
            return L"黒";
        }
        return L"灰色";
    }

    // 色相を 0..360 に正規化
    double hue = h;
    while (hue < 0) {
        hue += 360.0;
    }
    while (hue >= 360.0) {
        hue -= 360.0;
    }

    std::wstring base;
    if (hue >= 345.0 || hue < 20.0)      base = L"赤";
    else if (hue < 40.0)                 base = L"橙";
    else if (hue < 65.0)                 base = L"黄";
    else if (hue < 95.0)                 base = L"黄緑";
    else if (hue < 140.0)                base = L"緑";
    else if (hue < 170.0)                base = L"青緑";
    else if (hue < 200.0)                base = L"水色";
    else if (hue < 260.0)                base = L"青";
    else if (hue < 285.0)                base = L"藍";
    else if (hue < 320.0)                base = L"紫";
    else                                 base = L"桃";

    std::wstring mod;
    if (l < 0.20)      mod = L"暗い";
    else if (l > 0.80) mod = L"薄い";

    std::wstring satMod;
    if (s < 0.20) satMod = L"くすんだ";

    std::wstring name;
    if (!satMod.empty()) {
        name += satMod;
    }
    if (!mod.empty()) {
        name += mod;
    }
    name += base;

    return name;
}

// ============================================================================
// RGB -> XYZ
// ============================================================================

Xyz RgbToXyz(int r, int g, int b) {
    auto invGamma = [](double c) {
        c /= 255.0;
        if (c <= 0.04045) {
            return c / 12.92;
        }
        return pow((c + 0.055) / 1.055, 2.4);
    };

    double R = invGamma(r);
    double G = invGamma(g);
    double B = invGamma(b);

    Xyz out;
    out.x = R * 0.4124564 + G * 0.3575761 + B * 0.1804375;
    out.y = R * 0.2126729 + G * 0.7151522 + B * 0.0721750;
    out.z = R * 0.0193339 + G * 0.1191920 + B * 0.9503041;
    return out;
}

// ============================================================================
// XYZ -> Lab
// ============================================================================

Lab XyzToLab(const Xyz& xyz) {
    const double Xn = 0.95047;
    const double Yn = 1.0;
    const double Zn = 1.08883;

    auto f = [](double t) {
        const double d = 6.0 / 29.0;
        if (t > pow(d, 3)) {
            return cbrt(t);
        }
        return t / (3 * d * d) + 4.0 / 29.0;
    };

    double fx = f(xyz.x / Xn);
    double fy = f(xyz.y / Yn);
    double fz = f(xyz.z / Zn);

    Lab out;
    out.L = 116.0 * fy - 16.0;
    out.a = 500.0 * (fx - fy);
    out.b = 200.0 * (fy - fz);
    return out;
}

// ============================================================================
// RGB -> Lab
// ============================================================================

Lab RgbToLab(int r, int g, int b) {
    Xyz xyz = RgbToXyz(r, g, b);
    return XyzToLab(xyz);
}

// ============================================================================
// DeltaE2000（Lab 空間での色差）
// 説明:
//   CIEDE2000 の実装。第三者が理解しやすいように中間変数の意味を
//   コメントで補足している。
// ============================================================================

double DeltaE2000(const Lab& c1, const Lab& c2) {
    double L1 = c1.L;
    double a1 = c1.a;
    double b1 = c1.b;
    double L2 = c2.L;
    double a2 = c2.a;
    double b2 = c2.b;

    double avgLp = (L1 + L2) / 2.0;
    double C1 = sqrt(a1 * a1 + b1 * b1);
    double C2 = sqrt(a2 * a2 + b2 * b2);
    double avgC = (C1 + C2) / 2.0;

    double avgC7 = std::pow(avgC, 7.0);
    double denom = avgC7 + std::pow(25.0, 7.0);
    double G = 0.5 * (1.0 - std::sqrt(avgC7 / denom));

    double a1p = (1.0 + G) * a1;
    double a2p = (1.0 + G) * a2;

    double C1p = sqrt(a1p * a1p + b1 * b1);
    double C2p = sqrt(a2p * a2p + b2 * b2);
    double avgCp = (C1p + C2p) / 2.0;

    auto hp_f = [&](double x, double y) -> double {
        if (x == 0 && y == 0) {
            return 0.0;
        }
        double ang = Rad2Deg(atan2(y, x));
        if (ang < 0) {
            ang += 360.0;
        }
        return ang;
    };

    double h1p = hp_f(a1p, b1);
    double h2p = hp_f(a2p, b2);

    double dLp = L2 - L1;
    double dCp = C2p - C1p;

    double dhp;
    if (C1p * C2p == 0) {
        dhp = 0.0;
    } else {
        double diff = h2p - h1p;
        if (fabs(diff) <= 180.0) {
            dhp = diff;
        } else if (diff > 180.0) {
            dhp = diff - 360.0;
        } else {
            dhp = diff + 360.0;
        }
    }

    double dHp = 2.0 * sqrt(C1p * C2p) * sin(Deg2Rad(dhp / 2.0));

    double avgLp_minus50_sq = (avgLp - 50.0) * (avgLp - 50.0);
    double SL = 1.0 + (0.015 * avgLp_minus50_sq) / sqrt(20.0 + avgLp_minus50_sq);
    double SC = 1.0 + 0.045 * avgCp;

    double avgHp;
    if (C1p * C2p == 0) {
        avgHp = h1p + h2p;
    } else {
        double diff = fabs(h1p - h2p);
        if (diff <= 180.0) {
            avgHp = (h1p + h2p) / 2.0;
        } else {
            if (h1p + h2p < 360.0) {
                avgHp = (h1p + h2p + 360.0) / 2.0;
            } else {
                avgHp = (h1p + h2p - 360.0) / 2.0;
            }
        }
    }

    double T = 1.0
        - 0.17 * cos(Deg2Rad(avgHp - 30.0))
        + 0.24 * cos(Deg2Rad(2.0 * avgHp))
        + 0.32 * cos(Deg2Rad(3.0 * avgHp + 6.0))
        - 0.20 * cos(Deg2Rad(4.0 * avgHp - 63.0));

    double deltaTheta = 30.0 * exp(-pow((avgHp - 275.0) / 25.0, 2.0));
    double RC = 2.0 * sqrt(std::pow(avgCp, 7.0) / (std::pow(avgCp, 7.0) + std::pow(25.0, 7.0)));
    double RT = -sin(Deg2Rad(2.0 * deltaTheta)) * RC;

    double SH = 1.0 + 0.015 * avgCp * T;

    double KL = 1.0;
    double KC = 1.0;
    double KH = 1.0;

    double termL = dLp / (KL * SL);
    double termC = dCp / (KC * SC);
    double termH = dHp / (KH * SH);

    double deltaE = sqrt(termL * termL + termC * termC + termH * termH + RT * termC * termH);
    return deltaE;
}

// ============================================================================
// 色クラスタリング処理
// 説明:
//   取得したピクセル群から Lab を用いてクラスタリングし、代表色上位2つを返す。
//   戻り値は ((color1, name1), (color2, name2)) の形式。
// ============================================================================

std::pair<std::pair<COLORREF, std::wstring>, std::pair<COLORREF, std::wstring>>
GetTopTwoMergedClusters(const std::vector<COLORREF>& pixels) {
    struct PixelLab {
        COLORREF color;
        Lab      lab;
        double   distFromOrigin;
        std::wstring name;
    };

    std::vector<PixelLab> labPixels;
    labPixels.reserve(pixels.size());

    for (COLORREF c : pixels) {
        if (c == CLR_INVALID) {
            continue;
        }
        int r = GetRValue(c);
        int g = GetGValue(c);
        int b = GetBValue(c);
        Lab lab = RgbToLab(r, g, b);
        double dist = lab.L * lab.L + lab.a * lab.a + lab.b * lab.b;
        PixelLab pl{ c, lab, dist, MakePracticalJapaneseName(c) };
        labPixels.push_back(pl);
    }

    if (labPixels.empty()) {
        return { { CLR_INVALID, NO_COLOR_STR }, { CLR_INVALID, NO_COLOR_STR } };
    }

    std::sort(labPixels.begin(), labPixels.end(),
              [](const PixelLab& A, const PixelLab& B) {
                  if (A.distFromOrigin != B.distFromOrigin) {
                      return A.distFromOrigin < B.distFromOrigin;
                  }
                  if (A.lab.L != B.lab.L) {
                      return A.lab.L < B.lab.L;
                  }
                  if (A.lab.a != B.lab.a) {
                      return A.lab.a < B.lab.a;
                  }
                  if (A.lab.b != B.lab.b) {
                      return A.lab.b < B.lab.b;
                  }
                  return A.color < B.color;
              });

    struct Cluster {
        int   count = 0;
        long  sumR  = 0;
        long  sumG  = 0;
        long  sumB  = 0;
        Lab   lab   = { 0.0, 0.0, 0.0 };
        std::wstring name;
    };

    std::vector<Cluster> clusters;
    clusters.reserve(labPixels.size());

    for (const auto& p : labPixels) {
        bool assigned = false;
        for (auto& cl : clusters) {
            double dE = DeltaE2000(p.lab, cl.lab);
            if (dE <= DELTA_E_THRESHOLD) {
                int newCount = cl.count + 1;
                cl.sumR += (long)GetRValue(p.color);
                cl.sumG += (long)GetGValue(p.color);
                cl.sumB += (long)GetBValue(p.color);
                cl.lab.L = (cl.lab.L * cl.count + p.lab.L) / newCount;
                cl.lab.a = (cl.lab.a * cl.count + p.lab.a) / newCount;
                cl.lab.b = (cl.lab.b * cl.count + p.lab.b) / newCount;
                cl.count = newCount;
                assigned = true;
                break;
            }
        }
        if (!assigned) {
            Cluster nc;
            nc.count = 1;
            nc.sumR  = (long)GetRValue(p.color);
            nc.sumG  = (long)GetGValue(p.color);
            nc.sumB  = (long)GetBValue(p.color);
            nc.lab   = p.lab;
            nc.name  = p.name;
            clusters.push_back(nc);
        }
    }

    struct ResultCluster {
        int        count = 0;
        COLORREF   avgColor = CLR_INVALID;
        Lab        lab;
        std::wstring name;
    };

    std::vector<ResultCluster> allClusters;
    int totalPixels = 0;

    for (auto& cl : clusters) {
        if (cl.count <= 0) {
            continue;
        }
        totalPixels += cl.count;
        int avgR = (int)std::lround((double)cl.sumR / cl.count);
        int avgG = (int)std::lround((double)cl.sumG / cl.count);
        int avgB = (int)std::lround((double)cl.sumB / cl.count);
        COLORREF avg = RGB(avgR, avgG, avgB);
        Lab lab = RgbToLab(avgR, avgG, avgB);
        allClusters.push_back({ cl.count, avg, lab, cl.name });
    }

    if (totalPixels == 0) {
        return { { CLR_INVALID, NO_COLOR_STR }, { CLR_INVALID, NO_COLOR_STR } };
    }

    std::sort(allClusters.begin(), allClusters.end(),
              [](const ResultCluster& a, const ResultCluster& b) {
                  return a.count > b.count;
              });

    if ((int)allClusters.size() > TOP_K_CLUSTERS) {
        allClusters.resize(TOP_K_CLUSTERS);
    }

    struct Merged {
        int   count = 0;
        long  sumR  = 0;
        long  sumG  = 0;
        long  sumB  = 0;
        Lab   lab   = { 0.0, 0.0, 0.0 };
        std::wstring name;
    };

    std::vector<Merged> mergedList;

    for (const auto& rc : allClusters) {
        bool merged = false;
        for (auto& m : mergedList) {
            double dE = DeltaE2000(rc.lab, m.lab);
            if (dE <= MERGE_DELTA_E) {
                int newCount = m.count + rc.count;
                m.sumR += (long)GetRValue(rc.avgColor) * (long)rc.count;
                m.sumG += (long)GetGValue(rc.avgColor) * (long)rc.count;
                m.sumB += (long)GetBValue(rc.avgColor) * (long)rc.count;
                m.lab.L = (m.lab.L * m.count + rc.lab.L * rc.count) / newCount;
                m.lab.a = (m.lab.a * m.count + rc.lab.a * rc.count) / newCount;
                m.lab.b = (m.lab.b * m.count + rc.lab.b * rc.count) / newCount;
                m.count = newCount;
                merged = true;
                break;
            }
        }
        if (!merged) {
            Merged mm;
            mm.count = rc.count;
            mm.sumR  = (long)GetRValue(rc.avgColor) * (long)rc.count;
            mm.sumG  = (long)GetGValue(rc.avgColor) * (long)rc.count;
            mm.sumB  = (long)GetBValue(rc.avgColor) * (long)rc.count;
            mm.lab   = rc.lab;
            mm.name  = rc.name;
            mergedList.push_back(mm);
        }
    }

    std::vector<ResultCluster> finalClusters;
    finalClusters.reserve(mergedList.size());

    for (auto& m : mergedList) {
        int avgR = (int)std::lround((double)m.sumR / m.count);
        int avgG = (int)std::lround((double)m.sumG / m.count);
        int avgB = (int)std::lround((double)m.sumB / m.count);
        COLORREF avg = RGB(avgR, avgG, avgB);
        Lab lab = RgbToLab(avgR, avgG, avgB);
        finalClusters.push_back({ m.count, avg, lab, m.name });
    }

    std::vector<ResultCluster> filtered;
    for (auto& fc : finalClusters) {
        double share = (double)fc.count / (double)totalPixels;
        if (share >= MIN_GROUP_SHARE) {
            filtered.push_back(fc);
        }
    }

    if (filtered.empty()) {
        return { { CLR_INVALID, NO_COLOR_STR }, { CLR_INVALID, NO_COLOR_STR } };
    }

    std::sort(filtered.begin(), filtered.end(),
              [](const ResultCluster& a, const ResultCluster& b) {
                  return a.count > b.count;
              });

    std::pair<COLORREF, std::wstring> out1 = { CLR_INVALID, NO_COLOR_STR };
    std::pair<COLORREF, std::wstring> out2 = { CLR_INVALID, NO_COLOR_STR };

    if (filtered.size() >= 1) {
        out1 = { filtered[0].avgColor, filtered[0].name };
    }
    if (filtered.size() >= 2) {
        out2 = { filtered[1].avgColor, filtered[1].name };
    }

    return { out1, out2 };
}

// ============================================================================
// 共通描画関数
// 説明:
//   スウォッチ（色見本）と色名・RGB/HEX を描画する。
// ============================================================================

void DrawColorCluster(HDC hdc, HFONT hFontTitle, HFONT hFontSmall, int swX, int swY, COLORREF color, const std::wstring& name) {
    RECT swRect = { swX, swY, swX + SWATCH_SIZE, swY + SWATCH_SIZE };
    HBRUSH b = CreateSolidBrush(color);
    FillRect(hdc, &swRect, b);
    DeleteObject(b);
    FrameRect(hdc, &swRect, (HBRUSH)GetStockObject(WHITE_BRUSH));

    int textX = swRect.right + WINDOW_PADDING;

    HFONT old = (HFONT)SelectObject(hdc, hFontTitle);
    SetTextColor(hdc, RGB(255, 255, 255));
    SetBkMode(hdc, TRANSPARENT);

    std::wstring displayName = name.empty() ? NO_COLOR_STR : name;
    RECT nameRect = { textX, swY, textX + 300, swY + 24 };
    DrawTextW(hdc, displayName.c_str(), -1, &nameRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, hFontSmall);
    SetTextColor(hdc, RGB(200, 200, 200));

    std::wstringstream ss;
    if (color != CLR_INVALID) {
        ss << L"RGB: "
           << (int)GetRValue(color) << L","
           << (int)GetGValue(color) << L","
           << (int)GetBValue(color) << L"   "
           << ColorToHex(color);
    } else {
        ss << L"RGB: " << NO_COLOR_STR;
    }

    RECT rgbRect = { textX, swY + 26, textX + 300, swY + 26 + 20 };
    DrawTextW(hdc, ss.str().c_str(), -1, &rgbRect, DT_LEFT | DT_SINGLELINE | DT_VCENTER);

    SelectObject(hdc, old);
}

// ============================================================================
// エントリポイントとウィンドウプロシージャ
// ============================================================================

#pragma warning(push)
#pragma warning(disable:28251) // wWinMain の注釈に関する警告を局所的に抑制
int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int) {
#pragma warning(pop)
    EnableDpiAwarenessIfRequested();

    const wchar_t CLASS_NAME[] = L"BeMyColorWindowClass";

    WNDCLASS wc = {};
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    RegisterClass(&wc);

    HWND hwnd = CreateWindowEx(
        WS_EX_TOPMOST,
        CLASS_NAME,
        L"BE MY COLOR",
        WS_POPUP,
        20, 20,
        WINDOW_WIDTH,
        WINDOW_HEIGHT,
        nullptr,
        nullptr,
        hInstance,
        nullptr
    );

    if (!hwnd) {
        return 0;
    }

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static HFONT hFontTitle = nullptr;
    static HFONT hFontSmall = nullptr;
    static const UINT_PTR timerId = 1;

    static HWND hBtnTop    = nullptr;
    static HWND hBtnBottom = nullptr;
    static HWND hBtnMin    = nullptr;
    static HWND hBtnExit   = nullptr;

    switch (uMsg) {
    case WM_CREATE: {
        // フォント作成（UI 表示用）
        hFontTitle = CreateFontW(
            16, 0, 0, 0,
            FW_BOLD, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS,
            L"Meiryo UI"
        );

        hFontSmall = CreateFontW(
            12, 0, 0, 0,
            FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_SWISS,
            L"Meiryo UI"
        );

        int btnW = 30;
        int btnH = 22;
        int gap  = 6;
        int left = WINDOW_PADDING;
        int btnY = WINDOW_PADDING;

        hBtnTop = CreateWindowW(
            L"BUTTON", L"⤒",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            left, btnY, btnW, btnH,
            hwnd,
            (HMENU)(INT_PTR)BUTTON_ID_TOPMOST,
            (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE),
            nullptr
        );

        hBtnBottom = CreateWindowW(
            L"BUTTON", L"⤓",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            left + (btnW + gap), btnY, btnW, btnH,
            hwnd,
            (HMENU)(INT_PTR)BUTTON_ID_BOTTOM,
            (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE),
            nullptr
        );

        hBtnMin = CreateWindowW(
            L"BUTTON", L"—",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            left + 2 * (btnW + gap), btnY, btnW, btnH,
            hwnd,
            (HMENU)(INT_PTR)BUTTON_ID_MINIMIZE,
            (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE),
            nullptr
        );

        hBtnExit = CreateWindowW(
            L"BUTTON", L"✖",
            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
            left + 3 * (btnW + gap), btnY, btnW, btnH,
            hwnd,
            (HMENU)(INT_PTR)BUTTON_ID_EXIT,
            (HINSTANCE)GetWindowLongPtr(hwnd, GWLP_HINSTANCE),
            nullptr
        );

        SendMessageW(hBtnTop,    WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        SendMessageW(hBtnBottom, WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        SendMessageW(hBtnMin,    WM_SETFONT, (WPARAM)hFontSmall, TRUE);
        SendMessageW(hBtnExit,   WM_SETFONT, (WPARAM)hFontSmall, TRUE);

        SetTimer(hwnd, timerId, TIMER_INTERVAL_MS, nullptr);
        return 0;
    }

    case WM_TIMER: {
        if (wParam != timerId) {
            break;
        }

        POINT p;
        if (!GetCursorPos(&p)) {
            break;
        }

        int half = SAMPLE_N / 2;
        int w = SAMPLE_N;
        int h = SAMPLE_N;
        int startX = p.x - half;
        int startY = p.y - half;

        std::vector<COLORREF> pixels;
        if (!GetBlockViaBitBlt(startX, startY, w, h, pixels)) {
            break;
        }
        if (pixels.empty()) {
            break;
        }

        auto clusters = GetTopTwoMergedClusters(pixels);
        g_state.lastColor1 = clusters.first.first;
        g_state.lastName1  = clusters.first.second;
        g_state.lastColor2 = clusters.second.first;
        g_state.lastName2  = clusters.second.second;

        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);

        if (id == BUTTON_ID_EXIT) {
            PostQuitMessage(0);
            return 0;
        } else if (id == BUTTON_ID_MINIMIZE) {
            ShowWindow(hwnd, SW_MINIMIZE);
            return 0;
        } else if (id == BUTTON_ID_TOPMOST) {
            g_state.isTopMost = !g_state.isTopMost;
            SetWindowPos(
                hwnd,
                g_state.isTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE
            );
            return 0;
        } else if (id == BUTTON_ID_BOTTOM) {
            g_state.isTopMost = false;
            SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            SetWindowPos(hwnd, HWND_BOTTOM,   0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE);
            return 0;
        }
        break;
    }

    case WM_LBUTTONDOWN: {
        SetCapture(hwnd);
        g_state.dragging = true;
        int x = (short)LOWORD(lParam);
        int y = (short)HIWORD(lParam);
        g_state.dragOffset = { x, y };
        return 0;
    }

    case WM_MOUSEMOVE: {
        if (g_state.dragging && (wParam & MK_LBUTTON)) {
            POINT pt;
            GetCursorPos(&pt);
            int newX = pt.x - g_state.dragOffset.x;
            int newY = pt.y - g_state.dragOffset.y;
            SetWindowPos(
                hwnd,
                g_state.isTopMost ? HWND_TOPMOST : HWND_NOTOPMOST,
                newX, newY,
                0, 0,
                SWP_NOSIZE | SWP_NOACTIVATE
            );
        }
        return 0;
    }

    case WM_LBUTTONUP: {
        if (g_state.dragging) {
            g_state.dragging = false;
            ReleaseCapture();
        }
        return 0;
    }

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);

        RECT client;
        GetClientRect(hwnd, &client);

        HBRUSH bg = CreateSolidBrush(RGB(30, 30, 30));
        FillRect(hdc, &client, bg);
        DeleteObject(bg);

        int topButtonsHeight = WINDOW_PADDING + 22 + 6;

        DrawColorCluster(hdc, hFontTitle, hFontSmall, WINDOW_PADDING, topButtonsHeight, g_state.lastColor1, g_state.lastName1);

        if (g_state.lastColor2 != CLR_INVALID) {
            int secondTop = topButtonsHeight + SWATCH_SIZE + WINDOW_PADDING;
            DrawColorCluster(hdc, hFontTitle, hFontSmall, WINDOW_PADDING, secondTop, g_state.lastColor2, g_state.lastName2);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_DESTROY: {
        KillTimer(hwnd, timerId);
        if (hFontTitle) {
            DeleteObject(hFontTitle);
        }
        if (hFontSmall) {
            DeleteObject(hFontSmall);
        }
        PostQuitMessage(0);
        return 0;
    }

    case WM_NCHITTEST: {
        LRESULT hit = DefWindowProc(hwnd, uMsg, wParam, lParam);
        if (hit == HTCLIENT) {
            return HTCAPTION;
        }
        return hit;
    }

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }

    return DefWindowProc(hwnd, uMsg, wParam, lParam);
}
