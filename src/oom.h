#if !defined(OOM_H)
#define OOM_H

void attempt_oom_adjust(int oom_score, int *old_value);

#endif // OOM_H
