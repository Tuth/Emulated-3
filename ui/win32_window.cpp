// Win32 implementation of the panel window boundary; renderer and firmware are unchanged.
#include "x11_window.h"
#include "win32_keys.h"
#include <windows.h>
#include <windowsx.h>
#include <exception>

namespace eiii::ui {
struct x11_window::impl {
    x11_window *owner;
    HWND hwnd = nullptr;
    HINSTANCE module = nullptr;
    bool child = false, dragging_size = false;
    POINT size_start{};
    int start_w=0,start_h=0;
    canvas frame;
    explicit impl(x11_window *o): owner(o) {}
    static LRESULT CALLBACK proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) noexcept {
        try { return dispatch(hwnd,msg,wp,lp); }
        catch(std::exception const &e) { report(hwnd,e.what()); }
        catch(...) { report(hwnd,"Unknown panel exception"); }
        return 0;
    }
    static void report(HWND hwnd,char const *message) noexcept {
        // Never propagate C++ exceptions through a host's Win32 window procedure.
        KillTimer(hwnd,1);
        OutputDebugStringA("EIII Rack panel error: ");OutputDebugStringA(message);OutputDebugStringA("\n");
        auto *p=reinterpret_cast<impl *>(GetWindowLongPtr(hwnd,GWLP_USERDATA));
        try { if(p && p->owner->cb.error)p->owner->cb.error(message); } catch(...) { }
    }
    static LRESULT dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto *p = reinterpret_cast<impl *>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
        if (msg == WM_NCCREATE) {
            p = static_cast<impl *>(reinterpret_cast<CREATESTRUCT *>(lp)->lpCreateParams);
            p->hwnd = hwnd;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(p));
        }
        if (!p) return DefWindowProc(hwnd,msg,wp,lp);
        auto &o = *p->owner; auto &cb = o.cb;
        int x=GET_X_LPARAM(lp), y=GET_Y_LPARAM(lp);
        switch(msg) {
        case WM_TIMER: if(cb.tick) cb.tick(); return 0;
        case WM_CLOSE: if(cb.close_request) cb.close_request();
            return 0;
        case WM_SIZE: o.m_w=LOWORD(lp); o.m_h=HIWORD(lp); if(cb.resized && o.m_w && o.m_h) cb.resized(o.m_w,o.m_h);
            return 0;
        case WM_SETCURSOR: {
            POINT pt;GetCursorPos(&pt);ScreenToClient(hwnd,&pt);
            if(p->child && cb.resize_requested && pt.x>=o.m_w-18 && pt.y>=o.m_h-18){SetCursor(LoadCursor(nullptr,IDC_SIZENWSE));return TRUE;}
            break;
        }
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps; HDC dc=BeginPaint(hwnd,&ps);
            if(!p->frame.px.empty()) {
                BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth=p->frame.w; bi.bmiHeader.biHeight=-p->frame.h;
                bi.bmiHeader.biPlanes=1; bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
                StretchDIBits(dc,0,0,o.m_w,o.m_h,0,0,p->frame.w,p->frame.h,p->frame.px.data(),&bi,DIB_RGB_COLORS,SRCCOPY);
            }
            if(p->child && cb.resize_requested){HPEN pen=CreatePen(PS_SOLID,1,RGB(160,160,160));auto old=SelectObject(dc,pen);
                for(int i:{5,9,13}){MoveToEx(dc,o.m_w-i-2,o.m_h-2,nullptr);LineTo(dc,o.m_w-2,o.m_h-i-2);}SelectObject(dc,old);DeleteObject(pen);}
            EndPaint(hwnd,&ps);
            return 0;
        }
        case WM_LBUTTONDOWN: case WM_MBUTTONDOWN: case WM_RBUTTONDOWN:
            SetFocus(hwnd); SetCapture(hwnd);
            if(msg==WM_LBUTTONDOWN && p->child && cb.resize_requested && x>=o.m_w-18 && y>=o.m_h-18){
                p->dragging_size=true;p->size_start={x,y};ClientToScreen(hwnd,&p->size_start);p->start_w=o.m_w;p->start_h=o.m_h;return 0;
            }
            if(cb.down) cb.down(x,y,msg==WM_LBUTTONDOWN?1:msg==WM_MBUTTONDOWN?2:3);
            return 0;
        case WM_LBUTTONUP: case WM_MBUTTONUP: case WM_RBUTTONUP:
            if(p->dragging_size){p->dragging_size=false;ReleaseCapture();if(cb.resize_finished)cb.resize_finished();return 0;}
            if(cb.up) cb.up(x,y,msg==WM_LBUTTONUP?1:msg==WM_MBUTTONUP?2:3);
            ReleaseCapture();
            return 0;
        case WM_MOUSEMOVE:
            if(p->dragging_size && cb.resize_requested){POINT pt{x,y};ClientToScreen(hwnd,&pt);
                int dx=pt.x-p->size_start.x,dy=pt.y-p->size_start.y;
                int delta=std::abs(dx)>=std::abs(dy*1472/564)?dx:dy*1472/564;
                cb.resize_requested(p->start_w+delta,p->start_h+dy);return 0;}
            if(cb.move) cb.move(x,y);
            return 0;
        case WM_MOUSEWHEEL: {
            POINT pt{x,y}; ScreenToClient(hwnd,&pt);
            if(cb.wheel) cb.wheel(pt.x,pt.y,GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA);
            return 0;
        }
        case WM_KILLFOCUS: case WM_CANCELMODE: p->dragging_size=false; if(cb.focus_out) cb.focus_out();
            return 0;
        case WM_KEYDOWN: case WM_KEYUP: {
            if(msg==WM_KEYDOWN && (lp & (1LL<<30))) return 0;
            unsigned long k=0;
            if(wp>='0' && wp<='9') k=wp;
            else if(wp>=VK_NUMPAD0 && wp<=VK_NUMPAD9) k=XK_KP_0+wp-VK_NUMPAD0;
            else switch(wp) {
                case VK_RETURN:k=XK_Return;break; case VK_UP:k=XK_Up;break; case VK_DOWN:k=XK_Down;break;
                case VK_LEFT:k=XK_Left;break; case VK_RIGHT:k=XK_Right;break;
                case VK_ADD:case VK_OEM_PLUS:k=XK_plus;break;
                case VK_SUBTRACT:case VK_OEM_MINUS:k=XK_minus;break;
            }
            if(k && cb.key) cb.key(k,msg==WM_KEYDOWN);
            return 0;
        }
        }
        return DefWindowProc(hwnd,msg,wp,lp);
    }
};
x11_window::x11_window():m(new impl(this)) {}
x11_window::~x11_window(){close();delete m;}
bool x11_window::open(uintptr_t parent,int w,int h,char const *title,std::string &error) {
    close();
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCSTR>(impl::proc), &m->module);
    m->child=parent!=0;
    WNDCLASSA wc{}; wc.lpfnWndProc=impl::proc; wc.hInstance=m->module;
    wc.lpszClassName="Emulated3Panel"; wc.hCursor=LoadCursor(nullptr,IDC_ARROW);
    if(!RegisterClassA(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS) {error="Cannot register panel window"; return false;}
    DWORD style=parent ? WS_CHILD | WS_VISIBLE : WS_OVERLAPPEDWINDOW;
    RECT r{0,0,w,h}; if(!parent) AdjustWindowRect(&r,style,FALSE);
    m_w=w;m_h=h;
    m->hwnd=CreateWindowExA(0,wc.lpszClassName,title,style,parent?0:CW_USEDEFAULT,parent?0:CW_USEDEFAULT,r.right-r.left,r.bottom-r.top,reinterpret_cast<HWND>(parent),nullptr,wc.hInstance,m);
    if(!m->hwnd){error="Cannot create panel window";return false;} if(parent) SetTimer(m->hwnd,1,16,nullptr);
    show(true);return true;
}
void x11_window::close(){if(m->hwnd){KillTimer(m->hwnd,1);DestroyWindow(m->hwnd);m->hwnd=nullptr;
    UnregisterClassA("Emulated3Panel",m->module);}m->frame={};}
bool x11_window::is_open() const{return m->hwnd!=nullptr;}
int x11_window::fd() const{return -1;}
uintptr_t x11_window::window_id() const{return reinterpret_cast<uintptr_t>(m->hwnd);}
void x11_window::pump(){MSG msg;while(PeekMessage(&msg,m->hwnd,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessage(&msg);}}
void x11_window::present(canvas const &c,recti r){m->frame=c;RECT rr{r.x0,r.y0,r.x1,r.y1};InvalidateRect(m->hwnd,&rr,FALSE);UpdateWindow(m->hwnd);}
void x11_window::set_size(int w,int h){RECT r{0,0,w,h};if(!m->child) AdjustWindowRect(&r,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(m->hwnd,nullptr,0,0,r.right-r.left,r.bottom-r.top,SWP_NOMOVE|SWP_NOZORDER);}
void x11_window::show(bool visible){ShowWindow(m->hwnd,visible?SW_SHOW:SW_HIDE);}
void x11_window::set_aspect(int,int){} // Renderer scales to the client area.
}
