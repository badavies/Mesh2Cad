// SPDX-License-Identifier: GPL-3.0-or-later
#include "m2c/document/Document.h"

#include "TestMeshes.h"

using namespace m2c;
using namespace m2c::test;

namespace {
std::shared_ptr<const TriMesh> cube() {
    return std::make_shared<const TriMesh>(makeCube());
}
} // namespace

TEST_CASE("Document adds and finds objects", "[document]") {
    Document doc;
    const auto id = doc.addMesh("cube", cube());
    REQUIRE(id.has_value());
    const auto found = doc.find(*id);
    REQUIRE(found.has_value());
    CHECK(found->name == "cube");
    CHECK_FALSE(doc.find(*id + 1).has_value());
    CHECK(doc.objects().size() == 1);
}

TEST_CASE("Document rejects a null mesh", "[document]") {
    Document doc;
    const auto id = doc.addMesh("nothing", nullptr);
    REQUIRE_FALSE(id.has_value());
    CHECK(id.error().code == ErrorCode::InvalidArgument);
    CHECK_FALSE(doc.canUndo());
}

TEST_CASE("Document undo and redo", "[document]") {
    Document doc;
    CHECK(doc.undo().error().code == ErrorCode::InvalidState);
    CHECK(doc.redo().error().code == ErrorCode::InvalidState);

    const auto a = doc.addMesh("a", cube());
    const auto b = doc.addMesh("b", cube());
    REQUIRE((a && b));
    CHECK(doc.objects().size() == 2);

    REQUIRE(doc.undo().has_value());
    CHECK(doc.objects().size() == 1);
    CHECK_FALSE(doc.find(*b).has_value());
    CHECK(doc.canRedo());

    REQUIRE(doc.redo().has_value());
    CHECK(doc.objects().size() == 2);
    CHECK(doc.find(*b).has_value());

    // A new change after undo discards the redo history, and ids are never reused.
    REQUIRE(doc.undo().has_value());
    const auto c = doc.addMesh("c", cube());
    REQUIRE(c.has_value());
    CHECK(*c != *b);
    CHECK_FALSE(doc.canRedo());
}

TEST_CASE("Undo shares meshes instead of copying them", "[document]") {
    Document doc;
    const auto mesh = cube();
    REQUIRE(doc.addMesh("a", mesh).has_value());
    REQUIRE(doc.addMesh("b", mesh).has_value());
    // Owners: the local `mesh`, object a, object b, and the undo snapshot that still holds a.
    CHECK(mesh.use_count() == 4);
}

TEST_CASE("Steps group changes into one undo entry", "[document]") {
    Document doc;
    doc.beginStep("import two", "cli");
    REQUIRE(doc.addMesh("a", cube()).has_value());
    REQUIRE(doc.addMesh("b", cube()).has_value());
    doc.commitStep();
    CHECK(doc.objects().size() == 2);
    REQUIRE(doc.undoHistory().size() == 1);
    CHECK(doc.undoHistory().front().label == "import two");
    CHECK(doc.undoHistory().front().source == "cli");
    REQUIRE(doc.undo().has_value());
    CHECK(doc.objects().empty());
    CHECK(doc.redoHistory().front().label == "import two");
}

TEST_CASE("An aborted step restores the document", "[document]") {
    Document doc;
    const auto a = doc.addMesh("a", cube());
    REQUIRE(a.has_value());
    const auto before = doc.revision();
    doc.beginStep("fails");
    REQUIRE(doc.rename(*a, "renamed").has_value());
    REQUIRE(doc.addMesh("b", cube()).has_value());
    doc.abortStep();
    CHECK(doc.objects().size() == 1);
    CHECK(doc.find(*a)->name == "a");
    CHECK(doc.undoHistory().size() == 1);
    CHECK(doc.revision() > before); // observers must refresh
}

TEST_CASE("Selections follow their mesh", "[document]") {
    Document doc;
    const auto mesh = cube();
    const auto id = doc.addMesh("m", mesh);
    REQUIRE(id.has_value());
    auto faces = std::make_shared<MeshSelection>(12);
    faces->set(3);
    const auto sel = doc.add("s", SelectionData{*id, faces});
    REQUIRE(sel.has_value());

    // Wrong size or missing mesh is refused.
    CHECK_FALSE(doc.add("bad", SelectionData{*id, std::make_shared<MeshSelection>(5)}).has_value());
    CHECK(doc.add("bad", SelectionData{99, faces}).error().code == ErrorCode::NotFound);

    // Moving vertices keeps the faces, so the selection stays.
    std::vector<Vec3d> moved = mesh->positions();
    for (Vec3d& v : moved) v.z += 1.0;
    const auto shifted = std::make_shared<const TriMesh>(
        *TriMesh::create(std::make_shared<const std::vector<Vec3d>>(moved), mesh->sharedIndices()));
    REQUIRE(doc.replaceMesh(*id, shifted, {}).has_value());
    CHECK(doc.find(*sel).has_value());

    // New faces drop it.
    REQUIRE(doc.replaceMesh(*id, cube(), {}).has_value());
    CHECK_FALSE(doc.find(*sel).has_value());
    REQUIRE(doc.undo().has_value());
    CHECK(doc.find(*sel).has_value());

    // Removing the mesh removes its selection.
    REQUIRE(doc.remove(*id).has_value());
    CHECK(doc.objects().empty());
}

TEST_CASE("Deviations follow their mesh and reference", "[document]") {
    Document doc;
    const auto mesh = cube();
    const auto id = doc.addMesh("m", mesh);
    const auto ref = doc.addMesh("r", cube());
    REQUIRE(id.has_value());
    REQUIRE(ref.has_value());
    const auto field = *DeviationField::create(std::vector<double>(8, 0.1), 0.5);
    const auto dev = doc.add("d", DeviationData{*id, *ref, field});
    REQUIRE(dev.has_value());
    CHECK(std::string(doc.find(*dev)->kind()) == "deviation");

    // Wrong size, missing mesh or missing reference is refused.
    CHECK_FALSE(doc.add("bad", DeviationData{*id, *ref, *DeviationField::create({1.0}, 0.5)}));
    CHECK(doc.add("bad", DeviationData{99, *ref, field}).error().code == ErrorCode::NotFound);
    CHECK(doc.add("bad", DeviationData{*id, 99, field}).error().code == ErrorCode::NotFound);

    // Moving the vertices invalidates the distances, even with the same faces.
    std::vector<Vec3d> moved = mesh->positions();
    for (Vec3d& v : moved) v.z += 1.0;
    const auto shifted = std::make_shared<const TriMesh>(
        *TriMesh::create(std::make_shared<const std::vector<Vec3d>>(moved), mesh->sharedIndices()));
    REQUIRE(doc.replaceMesh(*id, shifted, {}).has_value());
    CHECK_FALSE(doc.find(*dev).has_value());
    REQUIRE(doc.undo().has_value());
    CHECK(doc.find(*dev).has_value());

    // So does changing or removing the reference.
    REQUIRE(doc.replaceMesh(*ref, cube(), {}).has_value());
    CHECK_FALSE(doc.find(*dev).has_value());
    REQUIRE(doc.undo().has_value());
    REQUIRE(doc.remove(*ref).has_value());
    CHECK_FALSE(doc.find(*dev).has_value());
    CHECK(doc.find(*id).has_value());
}
