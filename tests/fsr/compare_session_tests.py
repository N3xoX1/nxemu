"""Regression: distinguish display bilinear, applet capture and CAS strengths."""
import csv
import tempfile
import unittest
from pathlib import Path
from compare_session import summarize


class SessionTest(unittest.TestCase):
    def test_filter_phases_and_applet_exclusion(self):
        with tempfile.TemporaryDirectory() as temp:
            path = Path(temp)/"metrics.csv"
            rows = []
            for segment, filter_id, strength, target in ((0,1,0,"display"), (1,7,0,"display"),
                                                        (2,11,30,"display"), (3,11,70,"display"),
                                                        (4,8,0,"display"), (5,19,66,"display"), (6,9,0,"display"), (0,1,0,"applet")):
                for frame in range(200):
                    rows.append(dict(frame=segment*200+frame,backend="Vulkan",segment=segment,
                        implementation="standard",filter=filter_id,input_width=1280,input_height=720,
                        width=1920,height=1080,layers=1,cpu_ms=.002,gpu_ms=.04,
                        elapsed_s=segment*20+frame*.1,frame_interval_ms=4,offscreen_passes=0,
                        target=target,selected_filter=7 if filter_id==11 else 8 if filter_id==19 else filter_id,
                        sharpness_stored=25,cas_strength=strength,viewport_width=1280,viewport_height=720))
            with path.open("w",newline="") as f:
                writer=csv.DictWriter(f,fieldnames=rows[0].keys());writer.writeheader();writer.writerows(rows)
            result=summarize(path,3,1)
            self.assertEqual(len(result),7)
            self.assertEqual([r["filter_name"] for r in result],
                             ["Bilinear","Catmull-Rom","Catmull-Rom + CAS","Catmull-Rom + CAS", "Catmull-Rom fast", "Catmull-Rom fast + CAS", "ScaleForce fast"])
            self.assertEqual([r["cas_strength"] for r in result],["0","0","30","70","0","66","0"])
            self.assertTrue(all(r["samples"] < 200 for r in result))
            self.assertTrue(all(r["submission_fps"] == 250 for r in result))

    def test_downscale_selection_is_distinct_from_upscaler(self):
        with tempfile.TemporaryDirectory() as temp:
            path=Path(temp)/"area.csv"
            rows=[]
            for segment, actual, down, mode in [(0,20,1,"downscale"),(1,1,0,"downscale"),(2,1,1,"native")]:
                for frame in range(100):
                    rows.append(dict(frame=segment*100+frame,backend="Vulkan",segment=segment,
                        implementation="standard",filter=actual,selected_filter=5,downscale_filter=down,
                        scale_mode=mode,input_width=3840,input_height=2160,viewport_width=3840,viewport_height=2160,
                        width=1920,height=1080,layers=1,cpu_ms=.001,gpu_ms=.02,
                        elapsed_s=segment*10+frame*.1,frame_interval_ms=4,offscreen_passes=0,
                        target="display",sharpness_stored=25,cas_strength=0))
            with path.open("w",newline="") as f:
                writer=csv.DictWriter(f,fieldnames=rows[0].keys());writer.writeheader();writer.writerows(rows)
            result=summarize(path,1,1)
            self.assertEqual([r["filter_name"] for r in result],["Area","Bilinear","Bilinear"])
            self.assertEqual([r["scale_mode"] for r in result],["downscale","downscale","native"])
            self.assertTrue(all(r["selected_filter"]=="5" for r in result))


if __name__ == "__main__":
    unittest.main()
