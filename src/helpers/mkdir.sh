#!/bin/sh
## 
##  mkdir.sh -- make directory hierarchy
##
##  Based on `mkinstalldirs' from Noah Friedman <friedman@prep.ai.mit.edu>
##  as of 1994-03-25, which was placed in the Public Domain.
##  Cleaned up for Apache's Autoconf-style Interface (APACI)
##  by Ralf S. Engelschall <rse@apache.org>
##

errstatus=0
for file in ${1+"$@"} ; do 
    set fnord `echo ":$file" |\
               sed -ne 's/^:\//#/;s/^://;s/\// /g;s/^#/\//;p'`
    shift
    pathcomp=
    for d in ${1+"$@"}; do
        pathcomp="$pathcomp$d"
        case "$pathcomp" in
            -* ) pathcomp=./$pathcomp ;;
        esac
        if test ! -d "$pathcomp"; then
            echo "mkdir $pathcomp" 1>&2
            mkdir "$pathcomp" || errstatus=$?
        fi
        pathcomp="$pathcomp/"
    done
done
exit $errstatus

