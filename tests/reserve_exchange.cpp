#include "../src/policy/migration_executor.h"
#include "../src/policy/wear_leveling_policy.h"
#include <cassert>
#include <iostream>
#include <map>
#include <stdexcept>
#include <tuple>
using namespace RAID_Policy;
using namespace SSD_Components;

using Location = std::pair<unsigned int, LHA_type>;
Location location(const ZoneDirectory& d, uint64_t zone, uint64_t offset = 0) {
	unsigned int disk; LHA_type lba; d.Resolve_zone_lba(zone, offset, disk, lba);
	return {disk, lba};
}
MigrationTask plan(const ZoneDirectory& d, const MigrationOp& op) {
	MigrationTask task; task.Op = op;
	auto copies = [&](uint64_t source, uint64_t target, std::vector<StripeCopyPlan>& out) {
		for (const auto& stream : d.Written_block_offsets_by_stream(source)) for (auto block : stream.second) {
			StripeCopyPlan copy; copy.Stream_id = stream.first; copy.Stripe_offset = block; copy.Lba_count = 8;
			d.Resolve_zone_lba(source, block * 8, copy.Source_disk_id, copy.Source_lba);
			d.Resolve_zone_lba(target, block * 8, copy.Destination_disk_id, copy.Destination_lba);
			out.push_back(copy);
		}
	};
	copies(op.Hot_zone, op.Cold_zone, task.Copies);
	if (op.Reserve_zone != INVALID_ZONE_ID) copies(op.Cold_zone, op.Reserve_zone, task.Cold_copies);
	return task;
}

void exchange(unsigned mode) {
	ZoneDirectory d; d.Initialize(4, 16, 16, 8, 128, true, 48);
	d.Configure_popularity("LAST_EPOCH", 10);
	std::map<Location, int> storage;
	for (uint64_t z = 0; z < d.User_zone_count(); ++z) {
		d.Observe_write(z, 0, 16, true);
		for (unsigned b = 0; b < 2; ++b) storage[location(d, z, b * 8)] = 100 + z * 2 + b;
	}
	d.Observe_write(0, 0, 8, true); d.Advance_popularity(10);
	WearLevelingPolicy policy; policy.Initialize(4, 1e12, 0, 4, 1024);
	WearSnapshot wear; wear.Values = {.9, .5, .3, .1};
	auto decision = policy.Evaluate(d, wear);
	assert(decision.Migrations.size() == 1); // exactly one spare on old SSD
	auto op = decision.Migrations.front();
	assert(op.Hot_zone == 0 && op.Cold_zone == 3 && op.Reserve_zone == 8);
	assert(d.Find_empty_zone_on_ssd(3, {}) == INVALID_ZONE_ID);
	bool rejected = false; try { ZoneResolveResult r; d.Resolve(128, r); } catch (const std::out_of_range&) { rejected = true; }
	assert(rejected);
	for (unsigned round = 0; round < 2; ++round) {
		auto task = plan(d, op);
		auto hot = location(d, op.Hot_zone), cold = location(d, op.Cold_zone), spare = location(d, op.Reserve_zone);
		auto young_spare = location(d, d.Find_reserve_zone_on_ssd(3));
		int h = storage.at(hot), c = storage.at(cold);
		MigrationExecutor executor(2); MigrationLimits limits;
		limits.Pause_duration = 5;
		if (mode == 1) limits.Slice_sectors = 8;
		if (mode == 2) limits.Slice_time = 1;
		if (mode == 3) limits.Pause_on_read = true;
		executor.Configure_limits(limits);
		unsigned finished = 0;
		executor.Task_finished = [&](uint64_t zone, bool aborted) { assert(zone == op.Hot_zone && !aborted); ++finished; };
		executor.Start({task}, d, 10);
		assert(d.Migrating_zone_count() == 3);
		User_Request write, read, unrelated, overflow;
		write.Type = UserRequestType::WRITE; write.Start_LBA = op.Hot_zone * 16; write.SizeInSectors = 8; write.Stream_id = 0;
		read.Type = UserRequestType::READ; read.Start_LBA = op.Cold_zone * 16; read.SizeInSectors = 8; read.Stream_id = 0;
		unrelated.Type = UserRequestType::READ; unrelated.Start_LBA = 16; unrelated.SizeInSectors = 8; unrelated.Stream_id = 0;
		overflow.Type = UserRequestType::WRITE; overflow.Start_LBA = op.Cold_zone * 16; overflow.SizeInSectors = 8; overflow.Stream_id = 0;
		unsigned observed = 0;
		auto observe = [&](const MigrationTask&, const User_Request*) { ++observed; };
		assert(executor.Maybe_intercept(&write, {op.Hot_zone}, d, observe, 10) == MigrationExecutor::InterceptResult::BUFFERED);
		assert(executor.Maybe_intercept(&read, {op.Cold_zone}, d, observe, 10) == MigrationExecutor::InterceptResult::BUFFERED);
		assert(executor.Maybe_intercept(&overflow, {op.Cold_zone}, d, observe, 10) == MigrationExecutor::InterceptResult::BACKPRESSURE);
		assert(observed == 0);
		std::map<unsigned int, int> buffer;
		io_request_id_type active;
		unsigned io = 0, discarded = 0, failed_discards = 0;
		auto submit = [&](const StripeCopyPlan& copy, bool is_write, uint64_t) {
			assert(active.empty());
			if (!is_write) buffer[copy.Stripe_offset] = storage.at({copy.Source_disk_id, copy.Source_lba});
			else storage[{copy.Destination_disk_id, copy.Destination_lba}] = buffer.at(copy.Stripe_offset);
			active = std::to_string(++io); return active;
		};
		auto discard = [&](const StripeCopyPlan& copy, uint64_t) {
			// Force one invalidation retry: the phase may not finish before it succeeds.
			if (!failed_discards++) return false;
			assert(storage.at({copy.Source_disk_id, copy.Source_lba}) == storage.at({copy.Destination_disk_id, copy.Destination_lba}));
			storage.erase({copy.Source_disk_id, copy.Source_lba}); ++discarded; return true;
		};
		for (sim_time_type now = 10; executor.Has_inflight() && now < 500; ++now) {
			if (!active.empty()) { auto id = active; active.clear(); assert(executor.Notify_request_completed(id, d)); }
			assert(executor.Maybe_intercept(&unrelated, {1}, d, observe, now) == MigrationExecutor::InterceptResult::SUBMITTED);
			executor.Poll(d, submit, discard, now);
			if (executor.Has_inflight()) { assert(executor.Drain_replay_requests().empty()); assert(d.Migrating_zone_count() == 3); }
		}
		assert(!executor.Has_inflight() && finished == 1 && io == 8 && discarded == 4);
		assert(executor.Cold_stages_completed() == 1);
		assert(storage.at(location(d, op.Hot_zone)) == h && storage.at(location(d, op.Cold_zone)) == c);
		assert(location(d, op.Hot_zone) == cold && location(d, op.Cold_zone) == spare);
		assert(location(d, op.Reserve_zone) == hot && !storage.count(hot));
		assert(location(d, d.Find_reserve_zone_on_ssd(3)) == young_spare);
		assert(d.Find_reserve_zone_on_ssd(0) == op.Reserve_zone);
		assert(d.Duplicate_physical_location_count() == 0 && d.Migrating_zone_count() == 0);
		auto replay = executor.Drain_replay_requests();
		assert(replay.size() == 2 && replay[0].Request == &write && replay[1].Request == &read);
		assert(!replay[0].Complete_without_dispatch && !replay[1].Complete_without_dispatch);
		if (mode == 1) assert(executor.Sector_pauses() > 0);
		if (mode == 2) assert(executor.Time_pauses() > 0);
		if (mode == 3) assert(executor.Read_pauses() > 0);
		// Exchange another old zone with the now-young hot zone, reusing the rotated reserve.
		op.Cold_zone = op.Hot_zone; op.Hot_zone = 4;
	}
}

int main() {
	for (unsigned mode = 0; mode < 4; ++mode) exchange(mode);
	ZoneDirectory d; d.Initialize(4, 16, 16, 8, 128, true, 48);
	d.Observe_write(0, 0, 8, true); d.Configure_popularity("LAST_EPOCH", 10); d.Advance_popularity(10);
	WearLevelingPolicy p; p.Initialize(4, 1e12, 0, 1, 1024); WearSnapshot w; w.Values = {.9, .5, .3, .1};
	auto op = p.Evaluate(d, w).Migrations.at(0); assert(op.Reserve_zone == INVALID_ZONE_ID);
	MigrationExecutor executor; executor.Start({plan(d, op)}, d);
	auto abort = executor.Abort_all(d); assert(abort.Task_count == 1 && d.Migrating_zone_count() == 2);
	std::cout << "reserve exchange: two stages, rotating spares, data, pause/resume, replay, discard retries, abort passed\n";
}
