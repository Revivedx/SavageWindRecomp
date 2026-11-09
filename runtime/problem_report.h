// A problem report (runtime/problem_report.c): Options > Problem report, and the start screen's Prelaunch tab.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// Makes problem-report-<date>-<time>.zip in the data folder with the log of the latest run and the one before, the
// settings file and system.txt. No game files, no saves, nothing sent anywhere. Returns the file's name, "" if it
// couldn't be made.
const char *problem_report_make(void);
#ifdef __cplusplus
}
#endif
