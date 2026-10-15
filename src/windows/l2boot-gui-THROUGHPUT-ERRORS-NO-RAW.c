/* L2BOOT GUI POC2 CREATE-RDB + aggregate throughput; simplified write controls (source candidate).
   Place next to rdisk_host_winusb.exe. Does not modify the USB companion.
   Build: cl /O2 /W4 l2boot-gui-THROUGHPUT-ERRORS-NO-RAW.c /Fe:l2boot-gui.exe /link /SUBSYSTEM:WINDOWS comdlg32.lib user32.lib gdi32.lib shell32.lib
*/
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ID_IMAGE 101
#define ID_BROWSE 102
#define ID_RO 103
#define ID_PRI 105
#define ID_PRI_EDIT 106
#define ID_START 107
#define ID_STOP 108
#define ID_LOG 109
#define ID_STATE 110
#define ID_CLEAR 111
#define ID_HELP 112
#define ID_CREATE 113
#define ID_RATE_READ 114
#define ID_RATE_WRITE 115
#define ID_TRAFFIC_TOTAL 116
#define ID_CREATE_PATH 201
#define ID_CREATE_BROWSE 202
#define ID_CREATE_SIZE 203
#define ID_CREATE_EXPLAIN 204
#define TIMER_POLL 1
static HWND win, titlelabel, pathbox, browse, createbtn, ro, pri, priedit, start, stop, logbox, state, clearbtn;
static HWND rate_read, rate_write, traffic_total;
static HFONT font, fontbig;
static HANDLE child= NULL, outread= NULL;
static char exe[MAX_PATH], imagepath[32768], pending[4096];
static size_t pending_len;
static int connected, creating;
static char newpath[32768];
static unsigned long newmib;
/* The GUI reads sparse cumulative STAT records; never request individual
 * READ/WRITE log lines. The companion timestamps are its own monotonic ticks. */
static unsigned long long prev_read_bytes,prev_write_bytes;
static unsigned long long total_read_bytes,total_write_bytes,total_errors;
static unsigned long long prev_stat_ms;
static ULONGLONG last_stat_arrival;
static int have_stat_baseline;

static void text(HWND h, const char *s) { SetWindowTextA(h,s); }
static HWND control(const char *type,const char *title,DWORD style,int id) {
    HWND h=CreateWindowExA(0,type,title,WS_CHILD|WS_VISIBLE|style,0,0,10,10,win,(HMENU)(INT_PTR)id,GetModuleHandleA(NULL),NULL);
    SendMessageA(h,WM_SETFONT,(WPARAM)font,TRUE);
    return h;
}
static void show_speed(double read_mibs,double write_mibs) {
    char buf[100];
    sprintf(buf,"READ     %7.2f MiB/s",read_mibs);text(rate_read,buf);
    sprintf(buf,"WRITE    %7.2f MiB/s",write_mibs);text(rate_write,buf);
}
static void show_totals(void) {
    char buf[240];
    sprintf(buf,"Total READ %.1f MiB   |   WRITE %.1f MiB   |   Errors %llu",
            (double)total_read_bytes/1048576.0,
            (double)total_write_bytes/1048576.0,total_errors);
    text(traffic_total,buf);
}
static void stats_idle(void) {
    have_stat_baseline=0;
    show_speed(0.0,0.0);
}
static void stats_reset(void) {
    prev_read_bytes=prev_write_bytes=total_read_bytes=total_write_bytes=0;
    prev_stat_ms=last_stat_arrival=0;
    total_errors=0;
    stats_idle();show_totals();
}
static void consume_stats(const char *line) {
    unsigned long long ms,r,w,e;
    if(sscanf(line,"@L2STAT ms=%llu read=%llu write=%llu errors=%llu",
              &ms,&r,&w,&e)!=4)return;
    if(have_stat_baseline && ms>prev_stat_ms &&
       r>=prev_read_bytes && w>=prev_write_bytes) {
        double secs=(double)(ms-prev_stat_ms)/1000.0;
        show_speed((double)(r-prev_read_bytes)/(1048576.0*secs),
                   (double)(w-prev_write_bytes)/(1048576.0*secs));
    } else show_speed(0.0,0.0);
    total_read_bytes=r;total_write_bytes=w;total_errors=e;
    prev_read_bytes=r;prev_write_bytes=w;prev_stat_ms=ms;
    last_stat_arrival=GetTickCount64();
    have_stat_baseline=1;
    show_totals();
}
static void logline(const char *s) {
    int len=GetWindowTextLengthA(logbox);
    /* Cap log memory instead of growing forever during heavy I/O. */
    if (len>96000) { SendMessageA(logbox,EM_SETSEL,0,48000); SendMessageA(logbox,EM_REPLACESEL,FALSE,(LPARAM)""); len=GetWindowTextLengthA(logbox); }
    SendMessageA(logbox,EM_SETSEL,len,len);
    SendMessageA(logbox,EM_REPLACESEL,FALSE,(LPARAM)s);
    SendMessageA(logbox,EM_SCROLLCARET,0,0);
}
static void set_running(int yes) {
    EnableWindow(pathbox,!yes); EnableWindow(browse,!yes); EnableWindow(createbtn,!yes);
    EnableWindow(ro,!yes);
    EnableWindow(pri,!yes); EnableWindow(priedit,!yes && SendMessageA(pri,BM_GETCHECK,0,0)==BST_CHECKED);
    EnableWindow(start,!yes); EnableWindow(stop,yes);
    if(!yes) { connected=0; stats_idle(); text(state,"STOPPED  |  No active companion"); }
}
static void flush_pending(void) {
    if(pending_len) {
        pending[pending_len]=0;
        /* Telemetry is a private control record, NEVER a log line. */
        if(strncmp(pending,"@L2STAT ",8)==0) {
            consume_stats(pending);pending_len=0;return;
        }
        if(strstr(pending,"connected (fresh WinUSB session)")) {
            connected=1;stats_idle();
            text(state,"CONNECTED  |  Amiga USB session active");
        }
        if(strstr(pending,"waiting for 0525:a4ad") ||
           strstr(pending,"USB session ended") ||
           strstr(pending,"USB session lost")) {
            connected=0;stats_idle();
            text(state,"WAITING  |  USB device 0525:a4ad");
        }
        logline(pending); logline("\r\n"); pending_len=0;
    }
}
static void consume(const char *b,DWORD n) {
    DWORD i;
    for(i=0;i<n;i++) {
        char c=b[i];
        if(c=='\n') {flush_pending(); continue;}
        if(c=='\r') continue;
        if(pending_len>=sizeof(pending)-2) flush_pending();
        pending[pending_len++]=c;
    }
}
static void reap(void) {
    DWORD exitcode;
    if(!child) return;
    if(outread) {
        for(;;) {
            DWORD avail=0,got=0;
            char buf[2048];
            if(!PeekNamedPipe(outread,NULL,0,NULL,&avail,NULL) || !avail) break;
            if(!ReadFile(outread,buf,avail>sizeof(buf)?sizeof(buf):avail,&got,NULL)||!got) break;
            consume(buf,got);
        }
    }
    if(GetExitCodeProcess(child,&exitcode) && exitcode!=STILL_ACTIVE) {
        char msg[100];
        flush_pending();
        sprintf(msg,"\r\nCompanion exited (code %lu).\r\n",(unsigned long)exitcode); logline(msg);
        CloseHandle(child); child=NULL;
        if(outread) { CloseHandle(outread); outread=NULL; }
        set_running(0);
    }
}
static int quoted(char *dest,size_t cap,const char *src) {
    /* CommandLineToArgvW-compatible quoting for normal path/argument strings. */
    size_t used=strlen(dest),slashes=0;
    const char *p;
    if(used+2>=cap) return 0;
    dest[used++]='"';
    for(p=src;;p++) {
        size_t i, count;
        if(*p=='\\') {slashes++; continue;}
        count=slashes*((*p=='"'||!*p)?2:1);
        if(used+count+3>=cap) return 0;
        for(i=0;i<count;i++)dest[used++]='\\';
        slashes=0;
        if(!*p) break;
        if(*p=='"')dest[used++]='\\';
        dest[used++]=*p;
    }
    dest[used++]='"'; dest[used]=0;
    return 1;
}
static void launch(void) {
    char command[65536]="",priority[32];
    STARTUPINFOA si; PROCESS_INFORMATION pi;
    SECURITY_ATTRIBUTES sa;
    HANDLE writepipe=NULL;
    DWORD flags=CREATE_NO_WINDOW;
    int n;
    GetWindowTextA(pathbox,imagepath,sizeof(imagepath));
    if(!imagepath[0] || GetFileAttributesA(imagepath)==INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(win,"Choose an existing HDF image first.","L2BOOT",MB_OK|MB_ICONWARNING);return;
    }
    if(SendMessageA(pri,BM_GETCHECK,0,0)==BST_CHECKED) {
        GetWindowTextA(priedit,priority,sizeof(priority));
        n=atoi(priority);
        if(priority[0]==0 || n< -128 || n>127) {MessageBoxA(win,"Boot priority must be -128 through 127.","L2BOOT",MB_OK|MB_ICONWARNING);return;}
    }
    if(!quoted(command,sizeof(command),exe)) return;
    strcat(command," ");
    if(!quoted(command,sizeof(command),imagepath)) return;
    strcat(command," --stats"); /* aggregate stdout every 500 ms */
    if(SendMessageA(ro,BM_GETCHECK,0,0)==BST_CHECKED) strcat(command," --readonly");
    if(SendMessageA(pri,BM_GETCHECK,0,0)==BST_CHECKED) {
        strcat(command," --bootpri ");strcat(command,priority);
    }
    ZeroMemory(&sa,sizeof(sa)); sa.nLength=sizeof(sa); sa.bInheritHandle=TRUE;
    if(!CreatePipe(&outread,&writepipe,&sa,0)) goto error;
    SetHandleInformation(outread,HANDLE_FLAG_INHERIT,0);
    ZeroMemory(&si,sizeof(si));si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES;si.hStdOutput=writepipe;si.hStdError=writepipe;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    ZeroMemory(&pi,sizeof(pi));
    if(!CreateProcessA(exe,command,NULL,NULL,TRUE,flags,NULL,NULL,&si,&pi)) goto error;
    CloseHandle(writepipe);writepipe=NULL;
    CloseHandle(pi.hThread); child=pi.hProcess;
    pending_len=0; connected=0;
    stats_reset();
    logline("\r\n--- New companion process ---\r\n");
    text(state,"STARTING  |  Inspecting image");set_running(1);
    text(state,"STARTING  |  Inspecting image");
    return;
error:
    {char msg[180];sprintf(msg,"Cannot launch rdisk_host_winusb.exe (Win32 error %lu).",(unsigned long)GetLastError());MessageBoxA(win,msg,"L2BOOT",MB_OK|MB_ICONERROR);}
    if(writepipe)CloseHandle(writepipe);
    if(outread) {CloseHandle(outread);outread=NULL;}
}
static void browse_image(void) {
    OPENFILENAMEA of; char file[32768]="";
    GetWindowTextA(pathbox,file,sizeof(file));
    ZeroMemory(&of,sizeof(of));of.lStructSize=sizeof(of);of.hwndOwner=win;
    of.lpstrFilter="Amiga disk images (*.hdf;*.img)\0*.hdf;*.img\0All files (*.*)\0*.*\0";
    of.lpstrFile=file;of.nMaxFile=sizeof(file);of.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
    if(GetOpenFileNameA(&of))text(pathbox,file);
}

/* CREATE always invokes the companion's existing OFFLINE create-rdb command.
   No GUI disk-layout implementation; the standalone `create` mode is unused. */
static HWND dialog_control(HWND parent,const char *klass,const char *label,
                           DWORD style,int id,int x,int y,int w,int h) {
    HWND item=CreateWindowExA(0,klass,label,WS_CHILD|WS_VISIBLE|style,
                             x,y,w,h,parent,(HMENU)(INT_PTR)id,
                             GetModuleHandleA(NULL),NULL);
    if(item)SendMessageA(item,WM_SETFONT,(WPARAM)font,TRUE);
    return item;
}
static void choose_new_image(HWND dlg) {
    OPENFILENAMEA of;
    char path[32768]="";
    GetDlgItemTextA(dlg,ID_CREATE_PATH,path,sizeof(path));
    ZeroMemory(&of,sizeof(of));of.lStructSize=sizeof(of);of.hwndOwner=dlg;
    of.lpstrFilter="Amiga hard disk (*.hdf)\0*.hdf\0All files (*.*)\0*.*\0";
    of.lpstrFile=path;of.nMaxFile=sizeof(path);
    of.lpstrDefExt="hdf";
    of.Flags=OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if(GetSaveFileNameA(&of))SetDlgItemTextA(dlg,ID_CREATE_PATH,path);
}
static INT_PTR CALLBACK create_dialog(HWND dlg,UINT msg,WPARAM wp,LPARAM lp) {
    (void)lp;
    switch(msg) {
    case WM_INITDIALOG:
        dialog_control(dlg,"STATIC","IMAGE FILE",0,0,16,15,430,19);
        dialog_control(dlg,"EDIT","",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,
                       ID_CREATE_PATH,16,37,320,27);
        dialog_control(dlg,"BUTTON","Browse...",BS_PUSHBUTTON|WS_TABSTOP,
                       ID_CREATE_BROWSE,342,37,96,27);
        dialog_control(dlg,"STATIC","SIZE (MiB)",0,0,16,80,145,20);
        dialog_control(dlg,"EDIT","128",WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL|ES_NUMBER,
                       ID_CREATE_SIZE,16,102,110,27);
        dialog_control(dlg,"STATIC","Allowed: 2 - 2048 MiB",0,0,137,105,220,20);
        dialog_control(dlg,"STATIC",
                       "Creates an RDB disk with one unformatted UDH0 (FFS DOS/3) partition.\r\n"
                       "It will NOT overwrite an existing file. Format UDH0 on the Amiga.",
                       SS_LEFT,ID_CREATE_EXPLAIN,16,144,425,49);
        dialog_control(dlg,"BUTTON","Create RDB image",BS_DEFPUSHBUTTON|WS_TABSTOP,
                       IDOK,220,208,130,32);
        dialog_control(dlg,"BUTTON","Cancel",BS_PUSHBUTTON|WS_TABSTOP,
                       IDCANCEL,359,208,80,32);
        return TRUE;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case ID_CREATE_BROWSE:choose_new_image(dlg);return TRUE;
        case IDCANCEL:EndDialog(dlg,IDCANCEL);return TRUE;
        case IDOK: {
            char sizebuf[32],*end;
            unsigned long value;
            DWORD attr;
            GetDlgItemTextA(dlg,ID_CREATE_PATH,newpath,sizeof(newpath));
            GetDlgItemTextA(dlg,ID_CREATE_SIZE,sizebuf,sizeof(sizebuf));
            value=strtoul(sizebuf,&end,10);
            if(!newpath[0] || !sizebuf[0] || *end || value<2 || value>2048) {
                MessageBoxA(dlg,"Choose a destination and enter a size from 2 to 2048 MiB.",
                            "Create RDB image",MB_OK|MB_ICONWARNING);
                return TRUE;
            }
            attr=GetFileAttributesA(newpath);
            if(attr!=INVALID_FILE_ATTRIBUTES) {
                MessageBoxA(dlg,"The destination already exists. Select a new filename.\r\n"
                            "Existing images are never overwritten.",
                            "Create RDB image",MB_OK|MB_ICONWARNING);
                return TRUE;
            }
            newmib=value;
            EndDialog(dlg,IDOK);
            return TRUE;
        }
        }
        break;
    }
    return FALSE;
}
static void create_image(void) {
    /* In-memory empty dialog template; all child controls are native Win32. */
    struct { DLGTEMPLATE dt; WORD menu,klass,title; } tmpl;
    char command[65536]="", sizebuf[32],buf[256];
    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    HANDLE readpipe=NULL,writepipe=NULL;
    DWORD code=1,last_error=0;
    int success=0;
    INT_PTR choice;
    MSG msg;
    if(child || creating)return;
    ZeroMemory(&tmpl,sizeof(tmpl));
    tmpl.dt.style=WS_POPUP|WS_CAPTION|WS_SYSMENU|DS_MODALFRAME;
    tmpl.dt.dwExtendedStyle=0;
    tmpl.dt.cdit=0;
    tmpl.dt.x=0;tmpl.dt.y=0;tmpl.dt.cx=460;tmpl.dt.cy=255;
    choice=DialogBoxIndirectParamA(GetModuleHandleA(NULL),&tmpl.dt,win,
                                   create_dialog,0);
    if(choice!=IDOK)return;
    if(GetFileAttributesA(exe)==INVALID_FILE_ATTRIBUTES) {
        MessageBoxA(win,"rdisk_host_winusb.exe must be next to the GUI.",
                    "Create RDB image",MB_OK|MB_ICONERROR);
        return;
    }
    sprintf(sizebuf,"%lu",newmib);
    if(!quoted(command,sizeof(command),exe))return;
    strcat(command," create-rdb ");
    if(!quoted(command,sizeof(command),newpath))return;
    strcat(command," ");strcat(command,sizebuf);
    ZeroMemory(&sa,sizeof(sa));sa.nLength=sizeof(sa);sa.bInheritHandle=TRUE;
    if(!CreatePipe(&readpipe,&writepipe,&sa,0))goto finish;
    SetHandleInformation(readpipe,HANDLE_FLAG_INHERIT,0);
    ZeroMemory(&si,sizeof(si));si.cb=sizeof(si);
    si.dwFlags=STARTF_USESTDHANDLES;
    si.hStdOutput=writepipe;si.hStdError=writepipe;
    si.hStdInput=GetStdHandle(STD_INPUT_HANDLE);
    ZeroMemory(&pi,sizeof(pi));
    if(!CreateProcessA(exe,command,NULL,NULL,TRUE,CREATE_NO_WINDOW,
                       NULL,NULL,&si,&pi))goto finish;
    CloseHandle(writepipe);writepipe=NULL;
    CloseHandle(pi.hThread);
    creating=1;set_running(1);
    text(state,"CREATING  |  Writing RDB image...");
    logline("\r\n--- create-rdb ---\r\n");
    /* Pump UI while the offline subprocess allocates/flushed the HDF.
       WM_CLOSE is refused during creation to avoid abandoning the file. */
    for(;;) {
        DWORD wait=MsgWaitForMultipleObjects(1,&pi.hProcess,FALSE,INFINITE,QS_ALLINPUT);
        if(wait==WAIT_OBJECT_0)break;
        if(wait==WAIT_FAILED)break;
        while(PeekMessageA(&msg,NULL,0,0,PM_REMOVE)) {
            TranslateMessage(&msg);DispatchMessageA(&msg);
        }
    }
    GetExitCodeProcess(pi.hProcess,&code);
    CloseHandle(pi.hProcess);
    creating=0;set_running(0);
    while(readpipe) {
        DWORD n=0;
        if(!ReadFile(readpipe,buf,sizeof(buf)-1,&n,NULL)||!n)break;
        buf[n]=0;
        logline(buf);
    }
    if(code==0) {
        text(pathbox,newpath);
        text(state,"CREATED  |  RDB image ready (unformatted)");
        MessageBoxA(win,"RDB image created. UDH0 is unformatted.\r\n"
                    "Start the companion, then format the partition on AmigaOS.",
                    "L2BOOT",MB_OK|MB_ICONINFORMATION);
    } else {
        char err[128];
        sprintf(err,"create-rdb failed (exit code %lu). Check the activity log.",
                (unsigned long)code);
        MessageBoxA(win,err,"L2BOOT",MB_OK|MB_ICONERROR);
    }
    success=1;
finish:
    last_error=GetLastError();
    if(writepipe)CloseHandle(writepipe);
    if(readpipe)CloseHandle(readpipe);
    if(!success) {
        char err[160];
        sprintf(err,"Cannot start create-rdb (Win32 error %lu).",
                (unsigned long)last_error);
        MessageBoxA(win,err,"L2BOOT",MB_OK|MB_ICONERROR);
    }
}

static void layout(int w,int h) {
    int left=24, right=w-24, full=right-left, logs=h-335;
    if(full<100 || logs<40)return;
    MoveWindow(state,left,14,full,26,TRUE);
    MoveWindow(titlelabel,left,48,full,18,TRUE);
    MoveWindow(pathbox,left,70,full-208,27,TRUE);
    MoveWindow(browse,right-202,70,96,27,TRUE);
    MoveWindow(createbtn,right-96,70,96,27,TRUE);
    MoveWindow(ro,left,112,150,26,TRUE);
    MoveWindow(pri,left+155,112,135,26,TRUE);
    MoveWindow(priedit,left+290,112,55,25,TRUE);
    MoveWindow(start,left,153,145,36,TRUE);
    MoveWindow(stop,left+156,153,145,36,TRUE);
    MoveWindow(clearbtn,right-103,158,103,27,TRUE);
    MoveWindow(rate_read,left,207,full/2-8,28,TRUE);
    MoveWindow(rate_write,left+full/2,207,full/2,28,TRUE);
    MoveWindow(traffic_total,left,244,full,24,TRUE);
    MoveWindow(logbox,left,288,full,logs,TRUE);
}
static LRESULT CALLBACK wndproc(HWND hwnd,UINT m,WPARAM wp,LPARAM lp) {
    switch(m) {
    case WM_CREATE: {
        NONCLIENTMETRICSA nm; HDC dc;
        /* CreateWindowEx sends WM_CREATE before assigning its return value to global win.
           Child controls must use the actual HWND now, not NULL. */
        win=hwnd;
        ZeroMemory(&nm,sizeof(nm));nm.cbSize=sizeof(nm);
        SystemParametersInfoA(SPI_GETNONCLIENTMETRICS,sizeof(nm),&nm,0);
        font=CreateFontIndirectA(&nm.lfMessageFont);
        dc=GetDC(hwnd);ReleaseDC(hwnd,dc);
        state=control("STATIC","STOPPED  |  No active companion",SS_LEFT,ID_STATE);
        titlelabel=control("STATIC","DISK IMAGE",SS_LEFT,ID_HELP);
        pathbox=control("EDIT","",WS_BORDER|ES_AUTOHSCROLL,ID_IMAGE);
        browse=control("BUTTON","Browse...",BS_PUSHBUTTON,ID_BROWSE);
        createbtn=control("BUTTON","Create...",BS_PUSHBUTTON,ID_CREATE);
        ro=control("BUTTON","Read-only",BS_AUTOCHECKBOX,ID_RO);
        pri=control("BUTTON","Boot priority",BS_AUTOCHECKBOX,ID_PRI);
        priedit=control("EDIT","0",WS_BORDER|ES_AUTOHSCROLL,ID_PRI_EDIT);
        start=control("BUTTON","START",BS_DEFPUSHBUTTON,ID_START);
        stop=control("BUTTON","STOP",BS_PUSHBUTTON,ID_STOP);
        clearbtn=control("BUTTON","Clear log",BS_PUSHBUTTON,ID_CLEAR);
        rate_read=control("STATIC","READ        0.00 MiB/s",SS_LEFT,ID_RATE_READ);
        rate_write=control("STATIC","WRITE       0.00 MiB/s",SS_LEFT,ID_RATE_WRITE);
        traffic_total=control("STATIC","Total READ 0.0 MiB   |   WRITE 0.0 MiB   |   Errors 0",SS_LEFT,ID_TRAFFIC_TOTAL);
        logbox=control("EDIT","L2BOOT companion GUI  |  Errors + events only\r\n",WS_BORDER|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,ID_LOG);
        /* A standard EDIT defaults to a small insertion limit on some
         * Windows versions. Keep a bounded scrollback of useful errors. */
        SendMessageA(logbox,EM_SETLIMITTEXT,131072,0);
        fontbig=CreateFontA(19,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,
                            DEFAULT_PITCH|FF_DONTCARE,"Segoe UI");
        SendMessageA(rate_read,WM_SETFONT,(WPARAM)fontbig,TRUE);
        SendMessageA(rate_write,WM_SETFONT,(WPARAM)fontbig,TRUE);
        SendMessageA(ro,BM_SETCHECK,BST_CHECKED,0);
        EnableWindow(stop,FALSE);EnableWindow(priedit,FALSE);
        SetTimer(hwnd,TIMER_POLL,100,NULL);
        return 0;
    }
    case WM_SIZE:layout(LOWORD(lp),HIWORD(lp));return 0;
    case WM_TIMER:
        if(wp==TIMER_POLL) {
            reap();
            /* The 2 s idle header timeout can leave no telemetry record:
             * never leave an old nonzero speed frozen in the GUI. */
            if(child && last_stat_arrival &&
               GetTickCount64()-last_stat_arrival>1500u &&
               have_stat_baseline) stats_idle();
        }
        return 0;
    case WM_COMMAND:
        switch(LOWORD(wp)) {
        case ID_BROWSE:browse_image();return 0;
        case ID_CREATE:create_image();return 0;
        case ID_START:launch();return 0;
        case ID_PRI: EnableWindow(priedit,SendMessageA(pri,BM_GETCHECK,0,0)==BST_CHECKED);return 0;
        case ID_RO:return 0;
        case ID_STOP:
            if(child && MessageBoxA(hwnd,"Stop the companion? Do not stop while Amiga is writing to the image. This forcibly ends the process; it does NOT safely eject the Amiga volume.","L2BOOT",MB_YESNO|MB_ICONWARNING)==IDYES) {
                TerminateProcess(child,1);reap();
            }return 0;
        case ID_CLEAR:text(logbox,"");return 0;
        }break;
    case WM_CLOSE:
        if(creating) {MessageBoxA(hwnd,"Image creation is in progress. Wait for completion.","L2BOOT",MB_OK|MB_ICONINFORMATION);return 0;}
        if(child && MessageBoxA(hwnd,"Companion is still running. Force stop and exit? Do not exit during disk writes.","L2BOOT",MB_YESNO|MB_ICONWARNING)!=IDYES)return 0;
        if(child)TerminateProcess(child,1);
        DestroyWindow(hwnd);return 0;
    case WM_DESTROY:
        KillTimer(hwnd,TIMER_POLL);
        if(child)CloseHandle(child);
        if(outread)CloseHandle(outread);
        if(font)DeleteObject(font);
        if(fontbig)DeleteObject(fontbig);
        PostQuitMessage(0);return 0;
    }
    return DefWindowProcA(hwnd,m,wp,lp);
}
int WINAPI WinMain(HINSTANCE instance,HINSTANCE prev,LPSTR args,int show) {
    WNDCLASSA wc;MSG msg;char *slash;
    (void)prev;(void)args;
    GetModuleFileNameA(NULL,exe,sizeof(exe));
    slash=strrchr(exe,'\\');if(!slash)return 1;
    strcpy(slash+1,"rdisk_host_winusb.exe");
    ZeroMemory(&wc,sizeof(wc));wc.lpfnWndProc=wndproc;wc.hInstance=instance;
    wc.lpszClassName="L2BootGuiPoc1";wc.hCursor=LoadCursor(NULL,IDC_ARROW);
    wc.hIcon=LoadIcon(NULL,IDI_APPLICATION);wc.hbrBackground=(HBRUSH)(COLOR_BTNFACE+1);
    if(!RegisterClassA(&wc))return 1;
    win=CreateWindowExA(0,wc.lpszClassName,"L2BOOT  |  USB Remote Disk",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,
        CW_USEDEFAULT,CW_USEDEFAULT,780,650,NULL,NULL,instance,NULL);
    if(!win)return 1;
    ShowWindow(win,show);UpdateWindow(win);
    while(GetMessageA(&msg,NULL,0,0)>0){TranslateMessage(&msg);DispatchMessageA(&msg);}
    return (int)msg.wParam;
}
