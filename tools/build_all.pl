use Cwd qw(cwd);
my %target = (
 "peer_demo"      => [qw/esp32 esp32s3 esp32s2/],
 "doorbell_demo"  => [qw/esp32p4 esp32s3/],
 "openai_demo"    => [qw/esp32s3/],
 "videocall_demo" => [qw/esp32p4/]
 );

use Term::ANSIColor;

 my $cur = cwd();
 for my $t(keys %target) {
    my @board = @{$target{$t}};
    print "Start build for $t @board\n";
    for (@board) {
        chdir("$cur/$t");
        `rm -rf dependencies.lock; rm -rf build; rm -rf managed_components`;
        `idf.py set-target $_ 2>&1;idf.py build 2>&1`;
        my $f = `ls build/*.elf`;
        if ($f =~/elf/) {
            print color("green"),"Build for $t target $_ success\n", color("reset");
        } else {
            print color("red"), "Fail to build for $t target $_ success\n",  color("reset");
        }
    }
 }

