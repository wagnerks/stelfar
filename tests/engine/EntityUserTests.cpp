#include <cstdint>
#include <utility>
#include <vector>

#include "EngineTestFixture.h"
#include "ecss/Registry.h"
#include "renderModule/EntityUser.h"

namespace SFE::EngineTests {
	using Render::EntityUser;

	namespace {
		using Runs = std::vector<std::pair<uint32_t, uint32_t>>;

		// Words of a writer of `kind`: word 0 `first`, word 1 the next number, the rest 0 but the kind.
		EntityUser::Words wordsOf(const uint32_t kind, const uint32_t first) {
			EntityUser::Words words{};
			words[0] = first;
			words[1] = first + 1;
			words[EntityUser::kKind] = kind;
			return words;
		}

		// An entity of the engine's world with nothing on it, destroyed as the test ends: its words go with it.
		class Bare {
		public:
			Bare() : id(ECSHandler::registry().takeEntity()) {}
			~Bare() { ECSHandler::destroyEntity(id); }
			Bare(const Bare&) = delete;
			Bare& operator=(const Bare&) = delete;
			const ecss::EntityId id;
		};
	}

	// What is set is what is read back; an entity nobody wrote reads all 0.
	TEST(EntityUserWords, SetAndGet) {
		auto& store = EntityUser::instance();
		Bare written;
		Bare untouched;
		store.set(written.id, wordsOf(5, 11));
		EXPECT_EQ(store.get(written.id), wordsOf(5, 11));
		EXPECT_EQ(store.get(untouched.id), EntityUser::Words{});
		EXPECT_TRUE(store.buffer()) << "RenderSystem made the engine's";
		EXPECT_GE(store.capacity(), 4096u);
	}

	// Word 7 is the writer's kind: words without one are not taken.
	TEST(EntityUserWords, KindZeroIsRefused) {
		auto& store = EntityUser::instance();
		Bare entity;
		store.set(entity.id, wordsOf(0, 11));
		EXPECT_EQ(store.get(entity.id), EntityUser::Words{});
	}

	// A writer clears its own words only; a clear of no kind clears whatever is there.
	TEST(EntityUserWords, ClearOnlyItsOwnKind) {
		auto& store = EntityUser::instance();
		Bare entity;
		store.set(entity.id, wordsOf(5, 11));
		store.clear(entity.id, 6);
		EXPECT_EQ(store.get(entity.id), wordsOf(5, 11)) << "another kind's clear";
		store.clear(entity.id, 5);
		EXPECT_EQ(store.get(entity.id), EntityUser::Words{});
		store.set(entity.id, wordsOf(6, 21));
		store.clear(entity.id);
		EXPECT_EQ(store.get(entity.id), EntityUser::Words{});
	}

	// Two writers of one entity: the later one's words stand.
	TEST(EntityUserWords, LaterWriterWins) {
		auto& store = EntityUser::instance();
		Bare entity;
		store.set(entity.id, wordsOf(5, 11));
		store.set(entity.id, wordsOf(6, 21));
		EXPECT_EQ(store.get(entity.id), wordsOf(6, 21));
	}

	// An entity's words go as it does, before its id can be taken again: no frame between.
	TEST(EntityUserWords, GoneWithItsEntity) {
		auto& store = EntityUser::instance();
		const auto id = ECSHandler::registry().takeEntity();
		store.set(id, wordsOf(5, 11));
		ASSERT_EQ(store.get(id), wordsOf(5, 11));
		ECSHandler::destroyEntity(id);
		EXPECT_EQ(store.get(id), EntityUser::Words{});
	}

	TEST(EntityUserWords, RunsOfConsecutiveIds) {
		EXPECT_EQ(EntityUser::runsOf({ 20, 3, 4, 5, 9, 10, 4 }), (Runs{ { 3, 3 }, { 9, 2 }, { 20, 1 } }));
		EXPECT_TRUE(EntityUser::runsOf({}).empty());
		EXPECT_EQ(EntityUser::runsOf({ 7 }), (Runs{ { 7, 1 } }));
	}

	// An upload sends the ids written since the last one, as runs, and words written again unchanged are not written.
	TEST(EntityUserWords, UploadSendsOnlyWhatChanged) {
		EntityUser store;
		store.create(*Engine::instance()->rhi());
		for (const uint32_t id : { 1u, 2u, 3u, 7u }) {
			store.set(id, wordsOf(5, id));
		}
		EXPECT_EQ(store.pendingRuns(), (Runs{ { 1, 3 }, { 7, 1 } }));
		store.upload();
		EXPECT_TRUE(store.pendingRuns().empty());
		for (const uint32_t id : { 1u, 2u, 3u, 7u }) {
			store.set(id, wordsOf(5, id));
		}
		EXPECT_TRUE(store.pendingRuns().empty()) << "the same words again";
		store.clear(2, 5);
		EXPECT_EQ(store.pendingRuns(), (Runs{ { 2, 1 } }));
		store.destroy();
	}

	// An id past the buffer: the next upload makes one that holds it, a power of two, with every word kept.
	TEST(EntityUserWords, GrowsPastTheLargestId) {
		EntityUser store;
		store.create(*Engine::instance()->rhi());
		EXPECT_EQ(store.capacity(), 4096u);
		const auto first = store.buffer();
		store.set(10, wordsOf(5, 3));
		store.set(5000, wordsOf(5, 9));
		store.upload();
		EXPECT_EQ(store.capacity(), 8192u);
		EXPECT_TRUE(store.buffer());
		EXPECT_NE(store.buffer().id, first.id);
		EXPECT_EQ(store.get(10), wordsOf(5, 3));
		EXPECT_EQ(store.get(5000), wordsOf(5, 9));
		EXPECT_TRUE(store.pendingRuns().empty()) << "the new buffer was made from the whole store";
		store.destroy();
	}

	// Words written before the buffer exists are in it as it is made, and destroy takes every entity's words.
	TEST(EntityUserWords, WordsBeforeCreateAreUploaded) {
		EntityUser store;
		store.set(1, wordsOf(5, 1));
		store.set(3, wordsOf(5, 3));
		EXPECT_EQ(store.pendingRuns(), (Runs{ { 1, 1 }, { 3, 1 } }));
		EXPECT_EQ(store.capacity(), 0u);
		EXPECT_FALSE(store.buffer());
		store.upload();
		EXPECT_EQ(store.pendingRuns().size(), 2u) << "nothing to send them to yet";

		store.create(*Engine::instance()->rhi());
		EXPECT_EQ(store.capacity(), 4096u);
		EXPECT_TRUE(store.buffer());
		EXPECT_TRUE(store.pendingRuns().empty()) << "made from the store";
		EXPECT_EQ(store.get(1), wordsOf(5, 1));
		EXPECT_EQ(store.get(3), wordsOf(5, 3));

		store.destroy();
		EXPECT_FALSE(store.buffer());
		EXPECT_EQ(store.get(1), EntityUser::Words{});
	}
}
