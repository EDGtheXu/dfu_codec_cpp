// Tests for the RecordCodecBuilder port: record<>, fieldOf, optionalFieldOf,
// forGetter and the constructor-based builder form.
#include "test_support.hpp"

namespace {

using codec::Codec;
using codec::DataResult;
using codec::JsonOps;
using codec::JsonValue;
using codec::Value;
using codec::MapCodec;
using codec::RecordBuilder;
using codec::codecs::Int;
using codec::codecs::String;
using codec::fieldOf;
using codec::optionalFieldOf;
using codec::record;
using codec::recordCodec;
using codec::testing::decode;
using codec::testing::decodeError;
using codec::testing::encode;
using codec::testing::json;

struct Address {
  std::string city;
  std::string zip;

  bool operator==(const Address& other) const {
    return city == other.city && zip == other.zip;
  }
};

struct Person {
  std::string name;
  int32_t age = 0;
  std::vector<std::string> tags;
  std::optional<Address> address;

  bool operator==(const Person& other) const {
    return name == other.name && age == other.age && tags == other.tags &&
           address == other.address;
  }
};

// No default constructor: only the constructor form of `record` can build it.
struct Immutable {
  Immutable(std::string nameIn, int32_t ageIn) : name(std::move(nameIn)), age(ageIn) {}
  std::string name;
  int32_t age;

  bool operator==(const Immutable& other) const {
    return name == other.name && age == other.age;
  }
};

struct Ranged {
  std::string name;
  int32_t age = 0;
};

MapCodec<Address> addressCodec() {
  static const MapCodec<Address> codec =
      record<Address>(fieldOf("city", &Address::city, String),
                      fieldOf("zip", &Address::zip, String));
  return codec;
}

MapCodec<Person> personCodec() {
  static const MapCodec<Person> codec = record<Person>(
      fieldOf("name", &Person::name, String), fieldOf("age", &Person::age, Int),
      fieldOf("tags", &Person::tags, codec::listOf(String)),
      optionalFieldOf("address", &Person::address, addressCodec().codec()));
  return codec;
}

TEST(RecordCodecTest, DecodesAndEncodesAllFieldKinds) {
  const std::string text =
      R"({"name":"bob","age":42,"tags":["a","b"],"address":{"city":"x","zip":"1"}})";
  const Person person = decode(personCodec().codec(), text);
  EXPECT_EQ(person.name, "bob");
  EXPECT_EQ(person.age, 42);
  EXPECT_EQ(person.tags, std::vector<std::string>({"a", "b"}));
  ASSERT_TRUE(person.address.has_value());
  EXPECT_EQ(person.address->city, "x");
  EXPECT_EQ(encode(personCodec().codec(), person), text);
}

TEST(RecordCodecTest, OptionalFieldCanBeAbsent) {
  const Person person = decode(personCodec().codec(), R"({"name":"bob","age":1,"tags":[]})");
  EXPECT_FALSE(person.address.has_value());
  // An absent optional field is not written back out.
  EXPECT_EQ(encode(personCodec().codec(), person), R"({"name":"bob","age":1,"tags":[]})");
}

TEST(RecordCodecTest, MissingRequiredFieldsAreAllReported) {
  const std::string error = decodeError<Person>(personCodec().codec(), "{}");
  EXPECT_EQ(error,
            "No key name in MapLike[{}]; No key age in MapLike[{}]; No key tags in MapLike[{}]");
}

TEST(RecordCodecTest, FieldFailuresAreJoinedInDeclarationOrder) {
  const std::string error =
      decodeError<Person>(personCodec().codec(), R"({"name":1,"age":"x","tags":{}})");
  EXPECT_EQ(error, "Not a string: 1; Not a number: \"x\"; Not a json array: {}");
}

TEST(RecordCodecTest, FailedFieldsStillProduceAPartialObject) {
  const Codec<Ranged> codec = record<Ranged>(
      fieldOf("name", &Ranged::name, String), fieldOf("age", &Ranged::age, codec::intRange(0, 10)));
  const DataResult<Ranged> result =
      codec.parse(JsonOps::INSTANCE, json(R"({"name":"bob","age":99})"));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "Value 99 outside of range [0:10]");
  ASSERT_TRUE(result.valueOrPartial().has_value());
  EXPECT_EQ(result.valueOrPartial()->name, "bob");
  EXPECT_EQ(result.valueOrPartial()->age, 99);
}

TEST(RecordCodecTest, MapCodecConvertsImplicitlyToCodec) {
  const Codec<Address> codec = addressCodec();  // implicit MapCodecCodec conversion
  EXPECT_EQ(decode(codec, R"({"city":"c","zip":"z"})").city, "c");
}

TEST(RecordCodecTest, RecordCodecHelperReturnsACodecDirectly) {
  const Codec<Address> codec = recordCodec<Address>(
      fieldOf("city", &Address::city, String), fieldOf("zip", &Address::zip, String));
  EXPECT_EQ(encode(codec, Address{"c", "z"}), R"({"city":"c","zip":"z"})");
}

TEST(RecordCodecTest, ConstructorFormSupportsTypesWithoutDefaultConstructor) {
  const Codec<Immutable> codec = record<Immutable>(
      [](std::string name, int32_t age) { return Immutable(std::move(name), age); },
      fieldOf("name", &Immutable::name, String), fieldOf("age", &Immutable::age, Int));
  const Immutable decoded = decode(codec, R"({"name":"n","age":3})");
  EXPECT_EQ(decoded, Immutable("n", 3));
  EXPECT_EQ(encode(codec, Immutable("n", 3)), R"({"name":"n","age":3})");
}

TEST(RecordCodecTest, GetterOnlyFieldsRequireTheConstructorForm) {
  const Codec<Immutable> codec = record<Immutable>(
      [](std::string name, int32_t age) { return Immutable(std::move(name), age); },
      fieldOf("name", String).forGetter<Immutable>(
          std::function<std::string(const Immutable&)>(
              [](const Immutable& value) { return value.name; })),
      fieldOf("age", Int).forGetter<Immutable>(std::function<int32_t(const Immutable&)>(
          [](const Immutable& value) { return value.age; })));
  EXPECT_EQ(encode(codec, Immutable("n", 3)), R"({"name":"n","age":3})");
  EXPECT_EQ(decode(codec, R"({"name":"n","age":3})"), Immutable("n", 3));
}

TEST(RecordCodecTest, MapCodecForGetterBuildsAGetterOnlyField) {
  const Codec<Immutable> codec = record<Immutable>(
      [](std::string name, int32_t) { return Immutable(std::move(name), 0); },
      String.fieldOf("name").forGetter<Immutable>(std::function<std::string(const Immutable&)>(
          [](const Immutable& value) { return value.name; })),
      MapCodec<int32_t>::unit(0).forGetter<Immutable>(
          std::function<int32_t(const Immutable&)>([](const Immutable&) { return 0; })));
  // The unit map codec encodes nothing.
  EXPECT_EQ(encode(codec, Immutable("n", 5)), R"({"name":"n"})");
  EXPECT_EQ(decode(codec, R"({"name":"n"})"), Immutable("n", 0));
}

// A *required* nested record, unlike Person::address which is optional.
struct Company {
  Address address;
  bool operator==(const Company& other) const { return address == other.address; }
};

TEST(RecordCodecTest, RequiredNestedRecordsPropagateErrors) {
  const Codec<Company> codec =
      recordCodec<Company>(fieldOf("address", &Company::address, addressCodec().codec()));
  EXPECT_EQ(decode(codec, R"({"address":{"city":"c","zip":"z"}})").address.city, "c");
  EXPECT_EQ(decodeError<Company>(codec, R"({"address":{}})" ),
            "No key city in MapLike[{}]; No key zip in MapLike[{}]");
  EXPECT_EQ(decodeError<Company>(codec, "{}"), "No key address in MapLike[{}]");
  EXPECT_EQ(decodeError<Company>(codec, R"({"address":[1]})"), "Not a JSON object: [1]");
}

TEST(RecordCodecTest, OptionalNestedRecordsSwallowTheElementError) {
  // DFU's OptionalFieldCodec returns Optional.empty() when the element codec
  // fails, so a malformed optional record silently becomes an absent one.
  const Person person =
      decode(personCodec().codec(), R"({"name":"n","age":1,"tags":[],"address":{}})");
  EXPECT_FALSE(person.address.has_value());
  EXPECT_EQ(person.name, "n");
}

TEST(RecordCodecTest, FieldKeysAreReportedForCompression) {
  const std::vector<Value> keys = personCodec().keys(JsonOps::INSTANCE);
  // MapCodec.keys concatenates the encoder and decoder key streams.
  EXPECT_EQ(keys.size(), 8u);
  EXPECT_EQ(keys[0].asJson().dump(), "\"name\"");
  EXPECT_EQ(keys[3].asJson().dump(), "\"address\"");
  std::vector<std::string> unique;
  for (const Value& key : keys) {
    if (std::find(unique.begin(), unique.end(), key.asJson().dump()) == unique.end()) {
      unique.push_back(key.asJson().dump());
    }
  }
  EXPECT_EQ(unique.size(), 4u);
}

TEST(RecordCodecTest, EncodeFailuresAreCollectedByTheBuilder) {
  const Codec<int32_t> bounded = Int.flatComapMap<int32_t>(
      [](const int32_t& value) { return value; },
      [](const int32_t& value) -> DataResult<int32_t> {
        if (value > 100) {
          return DataResult<int32_t>::error("too large: " + std::to_string(value));
        }
        return DataResult<int32_t>::success(value);
      });
  const Codec<Immutable> codec = record<Immutable>(
      [](std::string name, int32_t age) { return Immutable(std::move(name), age); },
      fieldOf("name", &Immutable::name, String), fieldOf("age", &Immutable::age, bounded));
  const DataResult<Value> result = codec.encodeStart(JsonOps::INSTANCE, Immutable("n", 200));
  ASSERT_TRUE(result.isError());
  EXPECT_EQ(result.message(), "too large: 200");
}

TEST(RecordCodecTest, EmptyRecordWithNoFields) {
  struct Nothing {
    bool operator==(const Nothing&) const { return true; }
  };
  // record<O>() with no fields decodes from any object and encodes as {}.
  const Codec<Nothing> codec = record<Nothing>([] { return Nothing{}; });
  EXPECT_EQ(encode(codec, Nothing{}), "{}");
  EXPECT_TRUE(decode(codec, "{}") == Nothing{});
}

TEST(RecordCodecTest, CompressedRecordRoundTripsThroughAKeyList) {
  const Person person{"bob", 42, {"a"}, Address{"c", "z"}};
  const DataResult<Value> encoded = personCodec().codec().encodeStart(JsonOps::COMPRESSED, person);
  ASSERT_TRUE(encoded.result().has_value());
  // Keys are [name, age, tags, address]; optional members that are present fill
  // their slot, absent ones stay null.
  EXPECT_EQ(encoded.result()->asJson().dump(), R"(["bob",42,["a"],["c","z"]])");
  EXPECT_EQ(decode(personCodec().codec(), R"(["bob",42,["a"],["c","z"]])", JsonOps::COMPRESSED),
            person);
  EXPECT_EQ(decode(personCodec().codec(), R"(["bob",42,["a"],null])", JsonOps::COMPRESSED).address,
            std::nullopt);
}

}  // namespace
