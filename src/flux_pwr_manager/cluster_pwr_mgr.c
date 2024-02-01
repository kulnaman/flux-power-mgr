#if HAVE_CONFIG_H
#include "config.h"
#endif
#include "cluster_pwr_mgr.h"
#include "constants.h"
#include "flux_pwr_logging.h"
#include "job_hash.h"
#include "power_policies/uniform_pwr_policy.h"
#define MAX_HOSTNAME_SIZE 256
flux_t *flux_handle;
char **cluster_node_hostname_list;
int nodes_in_cluster;
cluster_mgr_t *self = NULL;
int global_power_ratio = 100; // Right now power ratio is global. All power is
                              // always distributed to GPUs.
int current_nodes_utilized = 0;

cluster_mgr_t *cluster_mgr_new(flux_t *h, double global_power_budget,
                               uint64_t cluster_size) {
  if (global_power_budget == 0 || cluster_size == 0)
    return NULL;

  cluster_node_hostname_list = calloc(cluster_size, sizeof(char *));
  if (!cluster_node_hostname_list)
    return NULL;

  cluster_mgr_t *cluster_mgr = calloc(1, sizeof(cluster_mgr_t));
  if (!cluster_mgr) {
    free(cluster_node_hostname_list);
    return NULL;
  }

  cluster_mgr->job_hash_table = job_hash_create();
  if (!cluster_mgr->job_hash_table) {
    free(cluster_node_hostname_list);
    free(cluster_mgr);
    return NULL;
  }

  zhashx_set_destructor(cluster_mgr->job_hash_table, job_map_destory);
  cluster_mgr->global_power_budget = global_power_budget;
  cluster_mgr->num_of_nodes = cluster_size;
  nodes_in_cluster = cluster_size;
  self = cluster_mgr;
  flux_handle = h;
  return cluster_mgr;
}
// Uniform power distributed
double redistribute_power(cluster_mgr_t *cluster_mgr, int num_of_nodes) {

  double new_per_node_powerlimit =
      cluster_mgr->global_power_budget / (num_of_nodes);

  void *data = zhashx_first(cluster_mgr->job_hash_table);

  while (data != NULL) {
    job_mgr_t *job_mgr_next = (job_mgr_t *)data;
    double new_job_powelimit =
        new_per_node_powerlimit * job_mgr_next->num_of_nodes;

    job_mgr_update_powerlimit(job_mgr_next, flux_handle, new_job_powelimit);

    void *data = zhashx_next(cluster_mgr->job_hash_table);
  }
  return new_per_node_powerlimit;
}
double get_new_job_powerlimit(cluster_mgr_t *cluster_mgr,
                              double num_of_requested_nodes_count) {

  int current_nodes = 0;
  double excess_power =
      (cluster_mgr->global_power_budget - cluster_mgr->current_power_usage);

  double theortical_power_per_node =
      excess_power / num_of_requested_nodes_count;

  double powerlimit_job = 0;
  // This is the case when power is access or just at max
  //
  if (theortical_power_per_node >= MAX_NODE_PWR) {
    powerlimit_job = MAX_NODE_PWR * num_of_requested_nodes_count;
  }
  // Not sufficient power for each node, each job has to suffer now.
  // Each job is getting total_pow/total_num_of_nodes.
  else if (theortical_power_per_node < MAX_NODE_PWR) {
    double new_per_node_powerlimit = redistribute_power(
        cluster_mgr, current_nodes_utilized + num_of_requested_nodes_count);
    powerlimit_job = new_per_node_powerlimit * num_of_requested_nodes_count;
  }
  cluster_mgr->current_power_usage += powerlimit_job;
  return powerlimit_job;
}

void cluster_mgr_add_hostname(int rank, char *hostname) {
  cluster_node_hostname_list[rank] = strdup(hostname);
}

int cluster_mgr_add_new_job(cluster_mgr_t *cluster_mgr, uint64_t jobId,
                            char **nodelist, int requested_nodes_count,
                            char *cwd, char *job_name) {
  if (jobId == 0 || !nodelist || !cwd || !cluster_mgr ||
      requested_nodes_count == 0)
    return -1;
  char **node_hostname_list;

  job_map_t *map = malloc(sizeof(job_map_t));
  if (!map)
    return -1;
  map->jobId = jobId;
  log_message("cluster_mgr requested node %d and jobId %ld",
              requested_nodes_count, jobId);
  int *ranks = calloc(requested_nodes_count, sizeof(int));
  // Quite inefficent but should be fine for newer systems
  for (int i = 0; i < requested_nodes_count; i++) {
    for (int j = 0; j < nodes_in_cluster; j++) {
      if (strcmp(nodelist[i], cluster_node_hostname_list[j]) == 0) {
        ranks[i] = j;
        break;
      }
    }
  }
  double job_pl = get_new_job_powerlimit(cluster_mgr, requested_nodes_count);
  map->job_pwr_manager =
      job_mgr_new(jobId, nodelist, requested_nodes_count, cwd, job_name,
                  UNIFORM, job_pl, global_power_ratio, ranks, flux_handle);
  if (!map->job_pwr_manager)
    return -1;
  if (!cluster_mgr->job_hash_table)
    return -1;
  log_message("Inserting data");
  zhashx_insert(cluster_mgr->job_hash_table, &map->jobId, (void *)map);
  current_nodes_utilized += requested_nodes_count;
  free(ranks);
  return 0;
}

int cluster_mgr_remove_job(cluster_mgr_t *cluster_mgr, uint64_t jobId) {
  log_message("cluster_mgr:remove job %ld", jobId);
  if (jobId == 0 || !cluster_mgr || !cluster_mgr->job_hash_table)
    return -1;
  job_map_t *job_map =
      (job_map_t *)zhashx_lookup(cluster_mgr->job_hash_table, &jobId);
  if (!job_map) {
    log_message("lookup failure");
    log_message("hash table size %d", zhashx_size(cluster_mgr->job_hash_table));
    log_message(
        "hash table first_element %ld",
        (uint64_t)zlistx_first(zhashx_keys(cluster_mgr->job_hash_table)));
    return -1;
  }

  current_nodes_utilized -= job_map->job_pwr_manager->num_of_nodes;
  log_message("cluster_mgr:remove job");
  zhashx_delete(cluster_mgr->job_hash_table, &job_map->jobId);
  return 0;
}

void cluster_mgr_destroy(cluster_mgr_t **manager) {
  if (!manager || !*manager)
    return;

  cluster_mgr_t *mgr = *manager;
  zhashx_purge(mgr->job_hash_table);
  zhashx_destroy(&mgr->job_hash_table);

  for (int i = 0; i < nodes_in_cluster; i++) {
    free(cluster_node_hostname_list[i]);
  }
  free(cluster_node_hostname_list);
  self = NULL;
  free(mgr);
  *manager = NULL; // Avoid dangling pointer
}

void job_map_destory(void **job_map) {
  log_message("Destroyer for job map called");
  if (job_map && *job_map) {
    job_map_t *proper_job_map = *job_map;
    job_mgr_destroy(flux_handle, &proper_job_map->job_pwr_manager);
    free(proper_job_map);
    *job_map = NULL; // Avoid dangling pointer
  }
}
int cluster_mgr_set_global_pwr_budget(cluster_mgr_t *cluster_mgr, double pwr) {
  if (!cluster_mgr)
    return -1;
  cluster_mgr->global_power_budget = pwr;
  return 0;
}

void cluster_mgr_set_global_powerlimit_cb(flux_t *h, flux_msg_handler_t *mh,
                                          const flux_msg_t *msg, void *args) {
  double powerlimit;
  if (flux_request_unpack(msg, NULL, "{s:f}", "gl_pl", &powerlimit) < 0) {
    log_error("RPC_ERROR:Unable to decode set powerlimit RPC");
  }
  if (powerlimit < 0)
    return;
  self->global_power_budget = powerlimit;
  redistribute_power(self, current_nodes_utilized);
}
void cluster_mgr_set_power_ratio_cb(flux_t *h, flux_msg_handler_t *mh,
                                    const flux_msg_t *msg, void *args) {

  int powerratio;
  if (flux_request_unpack(msg, NULL, "{s:i}", "pr", &powerratio) < 0) {
    log_error("RPC_ERROR:Unable to decode set poweratio RPC");
  }
  log_message("Setting Power Ratio of cluster to %d", powerratio);
  global_power_ratio = powerratio;
}