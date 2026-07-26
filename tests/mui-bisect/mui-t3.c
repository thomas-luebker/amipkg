/* mui-t3 — bisect step 3: t2 + the 4-column List with the amipkg display hook
 * (HookEntry + format string + title row + 3 entries). NO menustrip/timer.
 * Crash here = the list/display-hook layer. */
#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/mui.h>
#include <utility/hooks.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>
#include <stdio.h>

struct Library *MUIMasterBase;
struct IntuitionBase *IntuitionBase;

typedef struct { char id[64]; char flag[4]; char version[16]; char status[14]; } Row;
static Row rows[3];

static ULONG disp_func(struct Hook *h, char **array, Row *r)
{
    (void)h;
    if (r) { array[0]=r->flag; array[1]=r->id; array[2]=r->version; array[3]=r->status; }
    else   { array[0]=(char*)""; array[1]=(char*)"\033bPackage";
             array[2]=(char*)"\033bVersion"; array[3]=(char*)"\033bStatus"; }
    return 0;
}
static struct Hook disp_hook;

static int run(void)
{
    Object *app, *win, *lst, *bt;
    int i;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    MUIMasterBase = OpenLibrary((STRPTR)"muimaster.library", 19);
    if (!MUIMasterBase || !IntuitionBase) { printf("t3: libs missing\n"); return 10; }
    disp_hook.h_Entry = (HOOKFUNC)HookEntry;
    disp_hook.h_SubEntry = (HOOKFUNC)disp_func;
    printf("t3: building app with list...\n");
    app = ApplicationObject,
        MUIA_Application_Title, (ULONG)"mui-t3",
        MUIA_Application_Base,  (ULONG)"MUIT3",
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (ULONG)"mui-t3 - list test, close me",
            WindowContents, VGroup,
                Child, ListviewObject,
                    MUIA_Listview_List, lst = ListObject, InputListFrame,
                        MUIA_List_Format,      (ULONG)"MAW=2,MIW=30,,",
                        MUIA_List_Title,       TRUE,
                        MUIA_List_DisplayHook, (ULONG)&disp_hook,
                    End,
                End,
                Child, bt = SimpleButton("_Quit"),
            End,
        End,
    End;
    if (!app) { printf("t3: app creation FAILED\n"); goto out; }
    printf("t3: inserting rows...\n");
    for (i = 0; i < 3; i++) {
        sprintf(rows[i].id, "package-%d", i + 1);
        sprintf(rows[i].flag, "%s", i == 1 ? "*" : "");
        sprintf(rows[i].version, "1.%d", i);
        sprintf(rows[i].status, "%s", i == 1 ? "installed" : "");
        DoMethod(lst, MUIM_List_InsertSingle, (ULONG)&rows[i], MUIV_List_Insert_Bottom);
    }
    printf("t3: opening window...\n");
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(bt, MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    set(win, MUIA_Window_Open, TRUE);
    {
        ULONG opened = 0;
        get(win, MUIA_Window_Open, &opened);
        if (!opened) {
            printf("t3: window did NOT open (screen depth / MUI config?)\n");
            goto out;
        }
    }
    {
        ULONG sigs = 0;
        long zeros = 0;
        while (DoMethod(app, MUIM_Application_NewInput, (ULONG)&sigs)
               != (ULONG)MUIV_Application_ReturnID_Quit) {
            if (sigs) {
                zeros = 0;
                sigs = Wait(sigs | SIGBREAKF_CTRL_C);
                if (sigs & SIGBREAKF_CTRL_C) break;
            } else if (++zeros > 100) {
                /* Pathological empty-mask stream: yield instead of spinning
                 * (one machine showed 100% CPU here). */
                Delay(1);
                if (zeros > 5000) {
                    printf("t3: NewInput never yields signals - aborting loop\n");
                    break;
                }
            }
        }
    }
    set(win, MUIA_Window_Open, FALSE);
    printf("t3 OK\n");
out:
    if (app) MUI_DisposeObject(app);
    if (MUIMasterBase) CloseLibrary(MUIMasterBase);
    if (IntuitionBase) CloseLibrary((struct Library *)IntuitionBase);
    return 0;
}

static struct StackSwapStruct sss; static char *stk; static int rc;
int main(void)
{
    stk = (char *)AllocMem(131072, MEMF_ANY);
    if (!stk) return run();
    sss.stk_Lower = (APTR)stk; sss.stk_Upper = (ULONG)stk + 131072;
    sss.stk_Pointer = (APTR)((ULONG)stk + 131072);
    StackSwap(&sss); rc = run(); StackSwap(&sss);
    FreeMem(stk, 131072);
    return rc;
}
