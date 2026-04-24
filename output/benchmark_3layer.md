# 3-Layer Benchmark Report

- generated_at: `2026-04-24T16:53:08`
- data: `/root/gym/PLIN-Cloud-Edge-Device-Learned-Index/../PLIN-Cloud-Edge-Device-Learned-Index_odd/dataset/Data.txt`
- workload: `/root/gym/PLIN-Cloud-Edge-Device-Learned-Index/../PLIN-Cloud-Edge-Device-Learned-Index_odd/data/workload_log.csv`
- topology: `/root/gym/PLIN-Cloud-Edge-Device-Learned-Index/src/common/topology.yaml`
- queries_per_end: `1000`
- bench_wait_ms: `12000`

## Summary

| metric | value |
|---|---:|
| total_queries | 10000 |
| found | 10000 |
| not_found | 0 |
| wall_seconds_max_end | 158.030000 |
| aggregate_qps_parallel | 63.28 |
| sum_end_qps | 4861678.39 |

## Stage Distribution

| stage | count | percent |
|---|---:|---:|
| stage1_local | 1000 | 10.00% |
| stage2_hot_cache | 5769 | 57.69% |
| stage3_same_edge_plin | 1436 | 14.36% |
| stage4_cross_edge | 1795 | 17.95% |

## Per-End Results

| end | queries | found | s1 | s2 | s3 | s4 | seconds | qps | hot_cache |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1 | 1000 | 1000 | 1000 | 0 | 0 | 0 | 0.000206 | 4861520.00 | 34 |
| 2 | 1000 | 1000 | 0 | 641 | 359 | 0 | 31.559100 | 31.69 | 361 |
| 3 | 1000 | 1000 | 0 | 641 | 359 | 0 | 31.543400 | 31.70 | 361 |
| 4 | 1000 | 1000 | 0 | 641 | 359 | 0 | 31.592700 | 31.65 | 361 |
| 5 | 1000 | 1000 | 0 | 641 | 359 | 0 | 31.578600 | 31.67 | 361 |
| 6 | 1000 | 1000 | 0 | 641 | 0 | 359 | 157.965000 | 6.33 | 361 |
| 7 | 1000 | 1000 | 0 | 641 | 0 | 359 | 157.719000 | 6.34 | 361 |
| 8 | 1000 | 1000 | 0 | 641 | 0 | 359 | 157.373000 | 6.35 | 361 |
| 9 | 1000 | 1000 | 0 | 641 | 0 | 359 | 157.961000 | 6.33 | 393 |
| 10 | 1000 | 1000 | 0 | 641 | 0 | 359 | 158.030000 | 6.33 | 393 |
