#if !defined(OOM_H)
#define OOM_H

void attempt_oom_adjust(int oom_score);
void reset_oom_adjust();

#endif // OOM_H
