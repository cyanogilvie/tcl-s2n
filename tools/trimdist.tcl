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
	deps/aws-lc/generated-src		crypto_test_data.cc.tar.bz2
	deps/aws-lc/crypto				*.txt
	deps/aws-lc/crypto/hpke			test-vectors.json
	deps/aws-lc/crypto/fipsmodule	policydocs
	deps/aws-lc/util/fipstools/acvp/acvptool	test
	deps/aws-lc						vectors
	deps/aws-lc						testdata
	deps/aws-lc						testvectors
	deps/aws-lc						compliance
	deps/aws-lc						googletest
	deps/aws-lc						fuzz
	deps/aws-lc						*_tests.txt
	deps/aws-lc						*.go
	deps/s2n-tls					docs
	deps/s2n-tls					bindings
	deps/s2n-tls					fuzz
	deps/s2n-tls					compliance
	deps/s2n-tls					tests
} {
	lappend hitlist {*}[find $base $pattern fn {
		switch -glob -- $fn {
			*/CMakeLists.txt		continue
			deps/s2n-tls/tests		continue
		}

		file delete -force $fn
		if {[file isdirectory $fn]} {
			string cat $fn /
		} else {
			set fn
		}
	}]
}
# s2n-tls's tests, but for its feature probes (tests/features): its CMake compiles them to learn
# what the libcrypto supports, and without them assumes nothing, RSA-PSS (so a client's TLS 1.3)
# included.  The interned build insists they ran (S2N_ENFORCE_PROPER_LIBCRYPTO_FEATURE_PROBE)
foreach fn [glob -nocomplain -directory deps/s2n-tls/tests *] {
	if {[file tail $fn] eq "features"} continue
	file delete -force $fn
	lappend hitlist [if {[file isdirectory $fn]} {string cat $fn /} else {set fn}]
}
puts "hitlist:\n\t[join  $hitlist \n\t]"
