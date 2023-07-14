#ifndef FLUX_JOB_POWER_UTIL_H
#define FLUX_JOB_POWER_UTIL_H
#include "dynamic_job_map.h"
#include "flux/core.h"
#include "job_data.h"
#include <jansson.h>
int parse_power_payload( json_t *payload, job_data *job,uint64_t timestamp);
int parse_jobs(flux_t *h, json_t* jobs, dynamic_job_map *job_map) ;
#endif
