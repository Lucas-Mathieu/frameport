"""Vermillion (xyz.TheAviary.Vermillion, GitHub #200): the hand and brush vanish over the painting canvas.

The game shows a canvas being painted as its own compositor layer (a quad) and turns the canvas's scene renderer off
meanwhile. The Frame's runtime draws quad layers above every projection layer whatever the submission order, so the
canvas covers the hand and brush, which are drawn in the scene. The game has a fallback that draws the canvas as
ordinary scene geometry; Klownicle found that forcing it shows hand and brush again and paints correctly (#200).

In 1.7.2 (Cpp2IL) the choice is App.Components.PaintableMesh: HasCompositorLayerAvailable (read by the overlay
setter, ShowOrHideCompositorOverlay, ApplyTexturesToRenderer, RebuildPaintTargetsAndBuffer and
ShouldUseCompositorOverlay) and ShouldUseCompositorOverlay (LateUpdate: IsCompositorOverlayActive = it). Both return
false, so the game's own code keeps the canvas in the scene on both sides of the switch. Only for this game: the
methods are looked up by name, so another build simply gets no change."""
from __future__ import annotations

from ..base import register
from .unity_text_input import RET_FALSE, Il2cppReturnPatch

PACKAGE = "xyz.TheAviary.Vermillion"
PAINTABLE = "Assembly-CSharp/App/Components/PaintableMesh.cs"


class VermillionSceneCanvas(Il2cppReturnPatch):
    id = "frame.vermillion_scene_canvas"
    title = "Vermillion: paint canvases in the scene"
    description = ("Vermillion shows a canvas being painted as a separate compositor layer, which the Frame draws in "
                   "front of everything: the hand and brush disappear over the canvas while they still paint. "
                   "Rewrites PaintableMesh.HasCompositorLayerAvailable and ShouldUseCompositorOverlay -> false in "
                   "libil2cpp.so (found with Cpp2IL), so the game uses its own fallback and draws the canvas in the "
                   "scene. The canvas may look a little less sharp.")
    order = 49
    targets = {PAINTABLE: {"get_HasCompositorLayerAvailable": RET_FALSE, "get_ShouldUseCompositorOverlay": RET_FALSE}}
    check_name = "Vermillion canvas"

    def applies(self, a):
        return a.package == PACKAGE and "libil2cpp.so" in a.libs and a.abi == "arm64-v8a"

    def detect(self, a):
        return None  # catalog recipe only


register(VermillionSceneCanvas)
