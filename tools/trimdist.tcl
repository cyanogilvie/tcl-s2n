set dist_dir	[lindex $argv 0]
cd $dist_dir

proc find {base pattern var script} {
	upvar 1 $var qfn
	set res		{}
	set dirs	[list $base]
	while {[llength $dirs]} {
		set dirs	[lassign $dirs dir]
		foreach fn [glob -nocomplain -tails -directory $dir *] {
			set qfn	[file join $dir $fn]
			if {[string match $pattern $fn]} {
				lappend res [uplevel 1 $script]
			} elseif {[file isdirectory $qfn]} {
				lappend dirs $qfn
			}
		}
	}
	set res
}

set hitlist	{}
foreach {base pattern} {
	deps/aws-lc/third_party			wycheproof_testvectors
	deps/aws-lc/generated-src		crypto_test_data.cc
	deps/aws-lc/crypto				*.txt
	deps/aws-lc						vectors
	deps/aws-lc						testdata
	deps/aws-lc						testvectors
	deps/aws-lc						compliance
	deps/aws-lc						googletest
	deps/aws-lc						fuzz
	deps/aws-lc						*_tests.txt
	deps/aws-lc						*.go
	deps/s2n-tls					bindings
	deps/s2n-tls					fuzz
	deps/s2n-tls					compliance
} {
	lappend hitlist {*}[find $base $pattern fn {
		switch -glob -- $fn {
			*/CMakeLists.txt		continue
		}

		file delete -force $fn
		if {[file isdirectory $fn]} {
			string cat $fn /
		} else {
			set fn
		}
	}]
}
puts "hitlist:\n\t[join  $hitlist \n\t]"
