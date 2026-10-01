#include "spice-widget-priv.h"

static void fractional_physical_size(void)
{
    /* Compositor scale can be slightly below its printed decimal value. */
    const double scale = 1.4 - 1e-12;

    g_assert_cmpint(spice_display_physical_size(700, scale), ==, 980);
    g_assert_cmpint(spice_display_physical_size(962, scale), ==, 1347);
    g_assert_cmpint(spice_display_physical_size(700, 1.0), ==, 700);
    g_assert_cmpint(spice_display_physical_size(5, 1.5), ==, 8);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/display/fractional-physical-size", fractional_physical_size);
    return g_test_run();
}
