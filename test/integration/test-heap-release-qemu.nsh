@echo -off
heap-release-test.efi app heap-rel-leak.efi 5
heap-release-test.efi app heap-rel-c.efi 5
heap-release-test.efi app heap-rel-cxx-eh.efi 5
heap-release-test.efi app heap-rel-cxx-noeh.efi 5
heap-release-test.efi app heap-rel-c-min.efi 5
heap-release-test.efi app heap-rel-order.efi 5
heap-release-test.efi app heap-rel-ehptr.efi 5
heap-release-test.efi app heap-rel-cbexit-app.efi 5
heap-release-test.efi hold heap-rel-exit-drv.efi
heap-release-test.efi load heap-rel-cxx-drv.efi 10
heap-release-test.efi load heap-rel-cxx-svc.efi 10
heap-release-test.efi load heap-rel-dbg-drv.efi 10
heap-release-test.efi load heap-rel-fail-drv.efi 10
heap-release-test.efi load heap-rel-svcexit.efi 10
heap-release-test.efi load heap-rel-tplexit.efi 10
heap-release-test.efi load heap-rel-childrun.efi 10
heap-release-test.efi load heap-rel-cbexit.efi 5
echo HEAPREL_STARTED
heap-release-test.efi driver heap-rel-cxx-drv.efi 10
heap-release-test.efi driver heap-rel-cxx-svc.efi 10
heap-release-test.efi driver heap-rel-dbg-drv.efi 10
heap-release-test.efi fail heap-rel-fail-drv.efi 10
heap-release-test.efi fail heap-rel-svcexit.efi 10 heap-rel-svcexit
heap-release-test.efi fail heap-rel-tplexit.efi 10
heap-release-test.efi fail heap-rel-childrun.efi 10
heap-release-test.efi fail heap-rel-cbexit.efi 5
echo HEAPREL_DONE
