/* mui-t1 — bisect step 1: ONLY library opens. No MUI objects at all.
 * Crash here = environment/library problem, not our code. */
#include <exec/types.h>
#include <proto/exec.h>
#include <stdio.h>
struct Library *MUIMasterBase;
int main(void)
{
    MUIMasterBase = OpenLibrary((STRPTR)"muimaster.library", 19);
    if (!MUIMasterBase) { printf("t1: muimaster.library v19+ NOT found\n"); return 10; }
    printf("t1 OK: muimaster.library v%d.%d\n",
           MUIMasterBase->lib_Version, MUIMasterBase->lib_Revision);
    CloseLibrary(MUIMasterBase);
    return 0;
}
