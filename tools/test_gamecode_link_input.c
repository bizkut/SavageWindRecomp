extern int test_external(int);
extern int test_data;
extern int test_missing_weak __attribute__((weak));
extern int call_got(int);
__asm__(".text\n.globl call_got\n.type call_got,@function\n"
        "call_got:\n jmp *test_external@GOTPCREL(%rip)\n");

int game_modules[32] __attribute__((aligned(64)));
int game_module_count = 7;
int *game_sites = &test_missing_weak;
int game_site_count;
static int local_value = 3;
static int local_function(int x) { return x + local_value; }
int (*test_code_pointer)(int) = local_function;
int *test_weak_pointer = &test_missing_weak;

int game_version(void)
{
    return call_got(test_code_pointer(game_module_count)) + test_external(0) + test_data + 9 + game_site_count;
}
