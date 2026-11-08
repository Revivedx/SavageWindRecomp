// A problem report (runtime/problem_report.c): Options > Problem report, and the start screen's Prelaunch tab.
#pragma once

#ifdef __cplusplus
extern "C" {
#endif
// Makes problem-report-<date>-<time>/ in the data folder with the log of the latest run and the one before, the
// settings file and system.txt. No game files, no saves, nothing sent anywhere. Returns the folder's name, "" if it
// couldn't be made.
const char *problem_report_make(void);
// Shows that folder in the file manager (Explorer, Finder, ...)
void problem_report_show(const char *dir);
#ifdef __cplusplus
}
#endif
