if {"::tcltest" ni [namespace children]} {
	package require tcltest
	namespace import ::tcltest::*
}

::tcltest::loadTestedCommands
package require s2n

tcl::tm::path add [file join [file dirname [info script]] ../local/lib/tcl8/site-tcl]

