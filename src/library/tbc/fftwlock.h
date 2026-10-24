/******************************************************************************
 * fftwlock.h
 * lddecode-library - the lock on the planner of FFTW
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * SPDX-FileCopyrightText: 2026 vrunk11
 *
 * This file is part of ld-decode-tools.
 ******************************************************************************/

#ifndef FFTWLOCK_H
#define FFTWLOCK_H

#include <mutex>

// The planner of FFTW (the functions that make and destroy plans, and the ones that allocate for them) is not thread safe: only fftw_execute is. The
// code that makes plans (the Transform PAL decoders, one per thread) takes this lock around it, so that threads that start together (the workers of
// the playback of ld-analyse) do not plan at the same time, which corrupted the heap.
inline std::mutex &fftwPlannerMutex()
{
    static std::mutex mutex;
    return mutex;
}

#endif // FFTWLOCK_H
