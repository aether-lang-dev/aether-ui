// UIKit must initialise before an Aether builder creates gesture-bearing views.
extern int aether_ui_program_main(int argc, char **argv);
extern int aether_ui_ios_main(int argc, char **argv, int (*entry)(int, char **));
int main(int argc, char **argv) {
    return aether_ui_ios_main(argc, argv, aether_ui_program_main);
}
