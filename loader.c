#include <kos.h>
#include <assert.h>

char *fnpre;

int main(int argc, char **argv) {
    void *subelf;

    dbgio_enable();
    dbgio_dev_select("fb");
    dbgio_printf("\n\n\n\n\n\n\n             Loading...\n");
    thd_sleep(375);
    FILE* fntest = fopen("/pc/sm64.bin", "rb");
    if (NULL == fntest) {
        fntest = fopen("/cd/sm64.bin", "rb");
        assert(fntest);
        dbgio_printf("             using /cd for assets\n");
        fnpre = "/cd";
    } else {
        dbgio_printf("             using /pc for assets\n");
        fnpre = "/pc";
    }

    fclose(fntest);   /* left open, the exec'd game hangs in KOS init under Flycast */

    char binfn[256];
    sprintf(binfn, "%s/sm64.bin", fnpre);

    /* Map the sub-elf */
    ssize_t se_size = fs_load(binfn, &subelf);
    assert(subelf);

    /* Tell exec to replace us */
    arch_exec(subelf, se_size);

    /* Shouldn't get here */
    assert_msg(false, "exec call failed");

    return 0;
}


