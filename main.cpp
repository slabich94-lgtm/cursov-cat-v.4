#include <windows.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <vector>
#include <algorithm>

#pragma comment(lib, "dwmapi.lib")

// CursorCat v4 - realistic 3D-looking desktop cat.
// Native Win32/GDI, no Python or external runtime required.
// The cat is constrained to Windows taskbars and supports multiple monitors.

static constexpr int CAT_W = 126;
static constexpr int CAT_H = 96;
static constexpr COLORREF KEY = RGB(255, 0, 255);

static HWND g_hwnd = nullptr;
static NOTIFYICONDATAW g_tray{};
static float g_x = 300.0f;
static float g_y = 0.0f;
static float g_vx = 1.25f;
static float g_phase = 0.0f;
static int g_frame = 0;
static DWORD g_stateSince = 0;
static bool g_followCursor = false;

enum State { WALK, PAUSE, JUMP, SLEEP };
static State g_state = WALK;
static float g_jumpSX=0, g_jumpSY=0, g_jumpTX=0, g_jumpTY=0;
static DWORD g_jumpStart=0;

struct TaskSurface { RECT r{}; HMONITOR monitor{}; };
static std::vector<TaskSurface> g_taskbars;

static RECT VirtualDesktop() {
    return RECT{
        GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_XVIRTUALSCREEN)+GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN)+GetSystemMetrics(SM_CYVIRTUALSCREEN)
    };
}

static bool IsTaskbarWindow(HWND h) {
    if (!h) return false;
    wchar_t cls[128]{};
    GetClassNameW(h, cls, 127);
    return wcscmp(cls, L"Shell_TrayWnd") == 0 || wcscmp(cls, L"Shell_SecondaryTrayWnd") == 0;
}

static BOOL CALLBACK EnumTaskbarMonitors(HMONITOR hm, HDC, LPRECT, LPARAM) {
    MONITORINFO mi{}; mi.cbSize=sizeof(mi);
    if (!GetMonitorInfoW(hm, &mi)) return TRUE;
    TaskSurface s{}; s.monitor=hm;
    HWND best=nullptr;
    // Primary taskbar.
    HWND h=FindWindowW(L"Shell_TrayWnd", nullptr);
    if (h) {
        RECT r{}, inter{}; if (GetWindowRect(h,&r) && IntersectRect(&inter, &r, &mi.rcMonitor)) best=h;
    }
    // Secondary taskbars are top-level windows with this class.
    struct FindCtx { HMONITOR hm; HWND found; } ctx{hm,nullptr};
    EnumWindows([](HWND w, LPARAM lp)->BOOL {
        auto* c=reinterpret_cast<FindCtx*>(lp);
        if (!IsTaskbarWindow(w) || !IsWindowVisible(w)) return TRUE;
        HMONITOR hm2=MonitorFromWindow(w, MONITOR_DEFAULTTONEAREST);
        if (hm2==c->hm) { c->found=w; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.found) best=ctx.found;
    if (best) {
        GetWindowRect(best,&s.r);
        g_taskbars.push_back(s);
    } else {
        // Fallback: use the monitor work-area bottom edge.
        s.r=mi.rcWork;
        s.r.top=s.r.bottom-48;
        g_taskbars.push_back(s);
    }
    return TRUE;
}

static void RefreshTaskbars() {
    g_taskbars.clear();
    EnumDisplayMonitors(nullptr,nullptr,EnumTaskbarMonitors,0);
    if (g_taskbars.empty()) {
        RECT vd=VirtualDesktop(); TaskSurface s{}; s.r=vd; s.r.top=s.r.bottom-48; g_taskbars.push_back(s);
    }
}

static const TaskSurface& SurfaceForPoint(float x, float y) {
    POINT p{(LONG)x,(LONG)y};
    for (auto &s:g_taskbars) if (PtInRect(&s.r,p)) return s;
    HMONITOR hm=MonitorFromPoint(p,MONITOR_DEFAULTTONEAREST);
    for (auto &s:g_taskbars) if (s.monitor==hm) return s;
    return g_taskbars.front();
}

static float SurfaceY(const TaskSurface& s) {
    // Stand just above the visible taskbar. Works for top/left/right taskbars too,
    // but the common bottom taskbar gets the most natural placement.
    MONITORINFO mi{}; mi.cbSize=sizeof(mi); GetMonitorInfoW(s.monitor,&mi);
    const RECT& m=mi.rcMonitor;
    if (s.r.bottom >= m.bottom-8) return (float)(s.r.top-CAT_H+7);
    if (s.r.top <= m.top+8) return (float)(s.r.bottom-7);
    if (s.r.left <= m.left+8) return (float)(s.r.right-18-CAT_W);
    return (float)(s.r.left+11);
}

static float SurfaceMinX(const TaskSurface& s) {
    MONITORINFO mi{}; mi.cbSize=sizeof(mi); GetMonitorInfoW(s.monitor,&mi);
    if (s.r.bottom >= mi.rcMonitor.bottom-8 || s.r.top <= mi.rcMonitor.top+8)
        return (float)mi.rcMonitor.left;
    if (s.r.left <= mi.rcMonitor.left+8) return (float)(s.r.right+4);
    return (float)mi.rcMonitor.left;
}
static float SurfaceMaxX(const TaskSurface& s) {
    MONITORINFO mi{}; mi.cbSize=sizeof(mi); GetMonitorInfoW(s.monitor,&mi);
    if (s.r.bottom >= mi.rcMonitor.bottom-8 || s.r.top <= mi.rcMonitor.top+8)
        return (float)(mi.rcMonitor.right-CAT_W);
    if (s.r.left <= mi.rcMonitor.left+8) return (float)(s.r.right+4);
    return (float)(mi.rcMonitor.right-CAT_W);
}

static void SnapToSurface(float x) {
    const TaskSurface& s=SurfaceForPoint(x,g_y);
    float minx=SurfaceMinX(s), maxx=SurfaceMaxX(s);
    g_x=std::clamp(g_x,minx,maxx);
    g_y=SurfaceY(s);
}

static void BeginJumpTo(float tx, float ty) {
    g_jumpSX=g_x; g_jumpSY=g_y; g_jumpTX=tx; g_jumpTY=ty;
    g_jumpStart=GetTickCount(); g_state=JUMP; g_stateSince=g_jumpStart;
}

static void SetState(State s) {
    if (g_state==s) return;
    g_state=s; g_stateSince=GetTickCount();
}

static void CatEllipse(HDC dc, int l,int t,int r,int b,COLORREF outer,COLORREF inner) {
    // Soft faux-3D shading: concentric ellipses create volume without external assets.
    HBRUSH bo=CreateSolidBrush(outer); HBRUSH bi=CreateSolidBrush(inner);
    HGDIOBJ old=SelectObject(dc,bo); Ellipse(dc,l,t,r,b); SelectObject(dc,bi);
    int pad=std::max(2,(r-l)/9); Ellipse(dc,l+pad,t+pad,r-pad,b-pad);
    SelectObject(dc,old); DeleteObject(bo); DeleteObject(bi);
}

static void DrawCat(HDC dc) {
    const int W=CAT_W,H=CAT_H;
    HBRUSH bg=CreateSolidBrush(KEY); RECT rr{0,0,W,H}; FillRect(dc,&rr,bg); DeleteObject(bg);
    SetBkMode(dc,TRANSPARENT);

    bool moving=(g_state==WALK||g_state==PAUSE);
    float bob = moving ? std::sin(g_phase*2.0f)*1.8f : 0.0f;
    int by=(int)bob;
    int step=(g_state==WALK)?(((g_frame/5)&1)?4:-4):0;
    int jumpSquash=(g_state==JUMP)?-3:0;

    // Ground shadow.
    HBRUSH shadow=CreateSolidBrush(RGB(65,65,70));
    Ellipse(dc,18,78,109,93); DeleteObject(shadow);

    // Tail behind body.
    HPEN tail=CreatePen(PS_SOLID,9,RGB(72,72,76)); HPEN tailHi=CreatePen(PS_SOLID,4,RGB(112,112,116));
    HPEN oldP=(HPEN)SelectObject(dc,tail);
    int sway=(int)(std::sin(g_phase*0.8f)*9);
    Arc(dc,78,36+sway/3,125,89+sway,120,45,86,45);
    SelectObject(dc,tailHi);
    Arc(dc,78,36+sway/3,125,89+sway,120,45,86,45);
    SelectObject(dc,oldP); DeleteObject(tail); DeleteObject(tailHi);

    // Legs with subtle directional highlights.
    CatEllipse(dc,39+step,63+by+jumpSquash,57+step,91+by,RGB(46,46,49),RGB(102,102,106));
    CatEllipse(dc,79-step,63+by+jumpSquash,97-step,91+by,RGB(46,46,49),RGB(102,102,106));

    // Body, chest and head.
    CatEllipse(dc,35,40+by+jumpSquash,103,82+by,RGB(42,42,45),RGB(113,113,117));
    CatEllipse(dc,43,43+by+jumpSquash,94,78+by,RGB(63,63,67),RGB(145,145,149));

    // Ears.
    POINT e1[3]={{34,35+by},{40,7+by},{60,31+by}};
    POINT e2[3]={{76,31+by},{101,7+by},{105,36+by}};
    HBRUSH fur=CreateSolidBrush(RGB(86,86,91)); HBRUSH inner=CreateSolidBrush(RGB(150,105,115));
    SelectObject(dc,fur); Polygon(dc,e1,3); Polygon(dc,e2,3);
    SelectObject(dc,inner); POINT i1[3]={{40,29+by},{42,14+by},{53,29+by}}; POINT i2[3]={{84,29+by},{99,14+by},{100,30+by}}; Polygon(dc,i1,3); Polygon(dc,i2,3);
    DeleteObject(fur); DeleteObject(inner);

    CatEllipse(dc,31,19+by,108,68+by,RGB(40,40,43),RGB(126,126,131));
    // Facial plane highlight.
    HBRUSH face=CreateSolidBrush(RGB(166,166,170)); SelectObject(dc,face); Ellipse(dc,47,43+by,92,65+by); DeleteObject(face);

    // Eyes: dark sockets + amber irises + bright catchlights.
    HBRUSH socket=CreateSolidBrush(RGB(42,42,44)); HBRUSH iris=CreateSolidBrush(RGB(188,150,64)); HBRUSH pupil=CreateSolidBrush(RGB(25,25,25)); HBRUSH white=CreateSolidBrush(RGB(245,245,245));
    SelectObject(dc,socket); Ellipse(dc,44,31+by,62,45+by); Ellipse(dc,78,31+by,96,45+by);
    SelectObject(dc,iris); Ellipse(dc,47,33+by,59,43+by); Ellipse(dc,81,33+by,93,43+by);
    SelectObject(dc,pupil); Ellipse(dc,51,33+by,55,43+by); Ellipse(dc,85,33+by,89,43+by);
    SelectObject(dc,white); Ellipse(dc,48,34+by,51,37+by); Ellipse(dc,82,34+by,85,37+by);
    DeleteObject(socket); DeleteObject(iris); DeleteObject(pupil); DeleteObject(white);

    // Nose and mouth.
    HBRUSH nose=CreateSolidBrush(RGB(155,103,111)); SelectObject(dc,nose); Ellipse(dc,66,44+by,74,51+by); DeleteObject(nose);
    HPEN mouth=CreatePen(PS_SOLID,2,RGB(52,52,55)); oldP=(HPEN)SelectObject(dc,mouth);
    MoveToEx(dc,70,50+by,nullptr); LineTo(dc,70,56+by); MoveToEx(dc,70,55+by,nullptr); LineTo(dc,64,58+by); MoveToEx(dc,70,55+by,nullptr); LineTo(dc,76,58+by);
    // Whiskers.
    MoveToEx(dc,45,47+by,nullptr); LineTo(dc,9,42+by); MoveToEx(dc,45,52+by,nullptr); LineTo(dc,7,51+by); MoveToEx(dc,45,57+by,nullptr); LineTo(dc,10,61+by);
    MoveToEx(dc,95,47+by,nullptr); LineTo(dc,125,42+by); MoveToEx(dc,95,52+by,nullptr); LineTo(dc,128,51+by); MoveToEx(dc,95,57+by,nullptr); LineTo(dc,125,61+by);
    SelectObject(dc,oldP); DeleteObject(mouth);

    // Fur tufts/highlights.
    HPEN hi=CreatePen(PS_SOLID,1,RGB(190,190,194)); oldP=(HPEN)SelectObject(dc,hi);
    for(int i=0;i<7;i++) { MoveToEx(dc,39+i*6,25+by,nullptr); LineTo(dc,37+i*6,19+by); }
    SelectObject(dc,oldP); DeleteObject(hi);

    if(g_state==SLEEP) {
        HPEN zpen=CreatePen(PS_SOLID,2,RGB(75,75,80)); oldP=(HPEN)SelectObject(dc,zpen);
        MoveToEx(dc,46,38+by,nullptr); LineTo(dc,57,38+by); MoveToEx(dc,81,38+by,nullptr); LineTo(dc,92,38+by); SelectObject(dc,oldP); DeleteObject(zpen);
        SetTextColor(dc,RGB(85,85,90)); HFONT f=CreateFontW(15,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,DEFAULT_PITCH,L"Segoe UI"); oldP=(HPEN)SelectObject(dc,(HPEN)f);
        TextOutW(dc,103,10,L"Z",1); TextOutW(dc,113,1,L"z",1); SelectObject(dc,oldP); DeleteObject(f);
    }
}

static void Tick() {
    if(g_taskbars.empty() || (g_frame%120)==0) RefreshTaskbars();
    POINT mouse{}; GetCursorPos(&mouse);
    DWORD age=GetTickCount()-g_stateSince;

    if(g_state==JUMP) {
        float p=(GetTickCount()-g_jumpStart)/650.0f;
        if(p>=1.0f) { g_x=g_jumpTX; g_y=g_jumpTY; SetState(WALK); }
        else { float e=p*p*(3.0f-2.0f*p); float arc=std::sin(p*3.14159265f)*34.0f; g_x=g_jumpSX+(g_jumpTX-g_jumpSX)*e; g_y=g_jumpSY+(g_jumpTY-g_jumpSY)*e-arc; }
    } else {
        const TaskSurface& s=SurfaceForPoint(g_x+CAT_W/2,g_y+CAT_H/2);
        float minx=SurfaceMinX(s), maxx=SurfaceMaxX(s);
        g_y=SurfaceY(s);
        if(g_state==WALK) {
            if((std::rand()%1000)<8) g_vx=(std::rand()%2?1.0f:-1.0f)*(0.75f+(std::rand()%120)/100.0f);
            g_x+=g_vx;
            if(g_x<=minx) {g_x=minx;g_vx=std::fabs(g_vx);}
            if(g_x>=maxx) {g_x=maxx;g_vx=-std::fabs(g_vx);}
            // Occasionally pause like a real pet.
            if(age>7000 && (std::rand()%1000)<3) SetState(PAUSE);
        } else if(g_state==PAUSE) {
            if(age>900+(std::rand()%1700)) SetState(WALK);
            if(std::abs((float)mouse.x-(g_x+CAT_W/2))<55 && std::abs((float)mouse.y-(g_y+CAT_H/2))<120) SetState(WALK);
        }

        // If cursor is on another monitor, occasionally jump to that monitor's taskbar.
        if((std::rand()%1000)<5) {
            HMONITOR hm=MonitorFromPoint(mouse,MONITOR_DEFAULTTONEAREST);
            for(const auto& other:g_taskbars) if(other.monitor==hm && other.monitor!=s.monitor) {
                float tx=std::clamp((float)mouse.x-CAT_W/2,SurfaceMinX(other),SurfaceMaxX(other));
                float ty=SurfaceY(other); BeginJumpTo(tx,ty); break;
            }
        }
        // Wake when cursor approaches.
        float dx=(float)mouse.x-(g_x+CAT_W/2), dy=(float)mouse.y-(g_y+CAT_H/2);
        if(dx*dx+dy*dy<90.0f*90.0f && g_state==PAUSE) SetState(WALK);
    }

    g_phase+=0.14f; ++g_frame;
    SetWindowPos(g_hwnd,HWND_TOPMOST,(int)g_x,(int)g_y,CAT_W,CAT_H,SWP_NOACTIVATE|SWP_SHOWWINDOW);
    InvalidateRect(g_hwnd,nullptr,FALSE);
}

static void TrayAdd(){
    ZeroMemory(&g_tray,sizeof(g_tray)); g_tray.cbSize=sizeof(g_tray); g_tray.hWnd=g_hwnd; g_tray.uID=1; g_tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; g_tray.uCallbackMessage=WM_APP+1; g_tray.hIcon=LoadIconW(nullptr,IDI_APPLICATION); lstrcpyW(g_tray.szTip,L"CursorCat v4"); Shell_NotifyIconW(NIM_ADD,&g_tray);
}
static void TrayRemove(){Shell_NotifyIconW(NIM_DELETE,&g_tray);}
static void TrayMenu(){
    POINT p{}; GetCursorPos(&p); HMENU m=CreatePopupMenu(); AppendMenuW(m,MF_STRING,1001,L"Закрыть котика"); SetForegroundWindow(g_hwnd); TrackPopupMenu(m,TPM_RIGHTBUTTON,p.x,p.y,0,g_hwnd,nullptr); DestroyMenu(m);
}

LRESULT CALLBACK WndProc(HWND h,UINT msg,WPARAM w,LPARAM l){
    if(msg==WM_TIMER){Tick();return 0;}
    if(msg==WM_PAINT){PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);DrawCat(dc);EndPaint(h,&ps);return 0;}
    if(msg==WM_ERASEBKGND)return 1;
    if(msg==WM_APP+1){if(l==WM_RBUTTONUP||l==WM_CONTEXTMENU)TrayMenu();return 0;}
    if(msg==WM_COMMAND&&LOWORD(w)==1001){DestroyWindow(h);return 0;}
    if(msg==WM_DISPLAYCHANGE){RefreshTaskbars();return 0;}
    if(msg==WM_DESTROY){TrayRemove();PostQuitMessage(0);return 0;}
    return DefWindowProcW(h,msg,w,l);
}

int WINAPI wWinMain(HINSTANCE inst,HINSTANCE,PWSTR,int){
    std::srand((unsigned)std::time(nullptr)); RefreshTaskbars();
    if(!g_taskbars.empty()){ g_x=SurfaceMinX(g_taskbars[0])+80; g_y=SurfaceY(g_taskbars[0]); }
    WNDCLASSW wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=inst; wc.lpszClassName=L"CursorCatWindowV4"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); RegisterClassW(&wc);
    g_hwnd=CreateWindowExW(WS_EX_LAYERED|WS_EX_TRANSPARENT|WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_TOPMOST,wc.lpszClassName,L"CursorCat v4",WS_POPUP,(int)g_x,(int)g_y,CAT_W,CAT_H,nullptr,nullptr,inst,nullptr);
    if(!g_hwnd)return 1;
    SetLayeredWindowAttributes(g_hwnd,KEY,0,LWA_COLORKEY); ShowWindow(g_hwnd,SW_SHOWNOACTIVATE); TrayAdd(); SetTimer(g_hwnd,1,16,nullptr); g_stateSince=GetTickCount();
    MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0){TranslateMessage(&msg);DispatchMessageW(&msg);} return 0;
}
