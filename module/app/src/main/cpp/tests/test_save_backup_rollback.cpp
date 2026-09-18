#include <cassert>

#include "plugins/save_backup/rollback.h"

int main() {
    save_backup::RollbackResult complete;
    assert(complete.complete());
    assert(complete.evidence_preserved);

    save_backup::RollbackResult dat_failed;
    dat_failed.dat_restored = false;
    dat_failed.evidence_preserved = true;
    assert(!dat_failed.complete());

    save_backup::RollbackResult sidecar_failed;
    sidecar_failed.sidecar_restored = false;
    sidecar_failed.evidence_preserved = true;
    assert(!sidecar_failed.complete());

    save_backup::RollbackResult warehouse_failed;
    warehouse_failed.warehouse_restored = false;
    warehouse_failed.evidence_preserved = false;
    assert(!warehouse_failed.complete());
    assert(!warehouse_failed.evidence_preserved);

    save_backup::RollbackResult evidence_failed;
    evidence_failed.evidence_preserved = false;
    assert(!evidence_failed.complete());
    return 0;
}
