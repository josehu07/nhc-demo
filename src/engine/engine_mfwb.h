/**
 * Multi-factor cache mode (with write-back) implementation.
 *
 * Writes always follow Write-Back. Reads to dirty entries always go to
 * cache. Reads to other entries swtich between cache & core according
 * to `load_admit`. Reads populate lines into cache only if `data_admit`
 * is on (i.e., in workload probing stage).
 *
 * Monitor logic is implemented in `mf_monitor.c`. Must ensure that
 * the monitor has been initialized and started through
 * `ocf_mngt_mf_monitor_init()`.
 */

/*========== [Orthus FLAG BEGIN] ==========*/

#ifndef ENGINE_MFWB_H_
#define ENGINE_MFWB_H_



#include "../ocf_request.h"


int ocf_read_mfwb(struct ocf_request *req);


#endif /* ENGINE_MFWB_H_ */

/*========== [Orthus FLAG END] ==========*/
