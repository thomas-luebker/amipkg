/* mui-t2 — bisect step 2: minimal MUI app via the SAME macro/inline machinery
 * amipkg-mui uses: ApplicationObject + Window + Label + SimpleButton +
 * notifications + NewInput loop. NO list/hook/menustrip/timer.
 * Crash here = the object-macro / LP-inline / varargs layer. */
#include <exec/types.h>
#include <exec/memory.h>
#include <libraries/mui.h>
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/muimaster.h>
#include <clib/alib_protos.h>
#include <stdio.h>

struct Library *MUIMasterBase;
struct IntuitionBase *IntuitionBase;

static int run(void)
{
    Object *app, *win, *bt;
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((STRPTR)"intuition.library", 37);
    MUIMasterBase = OpenLibrary((STRPTR)"muimaster.library", 19);
    if (!MUIMasterBase || !IntuitionBase) { printf("t2: libs missing\n"); return 10; }
    printf("t2: building app...\n");
    app = ApplicationObject,
        MUIA_Application_Title, (ULONG)"mui-t2",
        MUIA_Application_Base,  (ULONG)"MUIT2",
        SubWindow, win = WindowObject,
            MUIA_Window_Title, (ULONG)"mui-t2 - close me",
            WindowContents, VGroup,
                Child, Label2("If you can read this, t2 works."),
                Child, bt = SimpleButton("_Quit"),
            End,
        End,
    End;
    if (!app) { printf("t2: app creation FAILED\n"); goto out; }
    printf("t2: app built, opening window...\n");
    DoMethod(win, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    DoMethod(bt, MUIM_Notify, MUIA_Pressed, FALSE,
             (ULONG)app, 2, MUIM_Application_ReturnID, MUIV_Application_ReturnID_Quit);
    set(win, MUIA_Window_Open, TRUE);
    {
        ULONG opened = 0;
        get(win, MUIA_Window_Open, &opened);
        if (!opened) {
            printf("t2: window did NOT open (screen depth / MUI config?)\n");
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
                    printf("t2: NewInput never yields signals - aborting loop\n");
                    break;
                }
            }
        }
    }
    set(win, MUIA_Window_Open, FALSE);
    printf("t2 OK\n");
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
