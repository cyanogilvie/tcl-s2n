# all.tcl --
#
# Runs every tests/*.test file in this process.  The script ends in an error when any test
# failed, so that tclsh exits non-zero and `meson test` (or make) sees the failure.

package require tcltest
namespace import ::tcltest::*

::tcltest::configure -singleproc true -testdir [file dirname [file normalize [info script]]] {*}$argv

# runAllTests returns whether anything failed from tcltest 2.5.5 (Tcl 8.6.13 / 9.0); before
# that it returns nothing, and failures can only be read from the output
set failed	[::tcltest::runAllTests]
if {$failed ne "" && $failed} {
	puts stderr "[file tail [info script]]: tests failed"
	# No exit here: it would leak what the script's evaluation still holds (noise under
	# valgrind).  Closing stderr keeps tclsh from adding a stack trace to the error
	close stderr
	error "test run failed"
}
return
