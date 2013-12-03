/**
 * @file backofftimer.h
 * @brief Generic timer facility with exponential backoff
 *
 * (c) 2013 by Mega Limited, Wellsford, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#ifndef MEGA_BACKOFF_TIMER_H
#define MEGA_BACKOFF_TIMER_H 1

#include "types.h"

namespace mega {

// generic timer facility with exponential backoff
class BackoffTimer
{
	dstime next;
	dstime delta;

public:
	// reset timer
	void reset();

	// trigger exponential backoff
	void backoff(dstime);

	// set absolute backoff
	void backoff(dstime, dstime);

	// check if timer has elapsed
	bool armed(dstime) const;

	// arm timer
	bool arm(dstime);

	// time left for event to become armed
	dstime retryin(dstime);

	// current backoff delta
	dstime backoff();

	// put on hold indefinitely
	void freeze();

	// time of next trigger or 0 if no trigger since last backoff
	dstime nextset() const;

	// update time to wait
	void update(dstime, dstime*);

	BackoffTimer();
};

} // namespace

#endif
