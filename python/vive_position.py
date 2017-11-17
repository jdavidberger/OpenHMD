#!/usr/bin/python3

import json
import pyvive
import signal

import cv2
import matplotlib.cm as cmx
import matplotlib.colors as colors
import matplotlib.pyplot as plt
import numpy as np
from mpl_toolkits.mplot3d import Axes3D
import time
import matplotlib

# The J axis (horizontal) sweep starts 71111 ticks after the sync
# pulse start (32°) and ends at 346667 ticks (156°).
# The K axis (vertical) sweep starts at 55555 ticks (23°) (25?) and ends
# at 331111 ticks (149°).

ticks_per_degree = 2222.22

angle_range_h = (32, 156)
angle_range_v = (25, 149)


class SigHandler:
    def __init__(self):
        self.quit = False
        signal.signal(signal.SIGINT, self.exit)
        signal.signal(signal.SIGTERM, self.exit)

    def exit(self, signum, frame):
        self.quit = True


class VivePos:
    def __init__(self):
        self.vive = pyvive.PyViveLibre()
        jet = plt.get_cmap('jet')
        c_norm = colors.Normalize(vmin=0, vmax=31)
        self.scalarMap = cmx.ScalarMappable(norm=c_norm, cmap=jet)

    @staticmethod
    def get_light_sensor_positions(config_text):
        j = json.loads(config_text)
        points = j["lighthouse_config"]["modelPoints"]
        normals = j["lighthouse_config"]["modelNormals"]
        return points, normals

    def draw_angles(self, angles_ax, angles):
        angles_ax.cla()
        angles_ax.set_title('Station View Angles (Degrees)')
        angles_ax.axis([40, (angle_range_v[1] - angle_range_v[0]) - 40,
                        40, (angle_range_h[1] - angle_range_h[0]) - 40])
        for i, a in angles.items():
            x = (angle_range_v[1] - angle_range_v[0]) - ((a[0] / ticks_per_degree) - angle_range_v[0])
            y = (a[1] / ticks_per_degree) - angle_range_h[0]

            angles_ax.scatter(x, y, color=self.scalarMap.to_rgba(i), s=2.0)

    def draw_config(self, config_ax, points, normals):
        config_vectors = []
        for i in range(0, 32):
            v = [points[i][0], -points[i][2], points[i][1],
                 normals[i][0], -normals[i][2], normals[i][1]]
            config_vectors.append(v)
        i = 0
        for p in points:
            config_ax.scatter(p[0], -p[2], p[1], color=self.scalarMap.to_rgba(i))
            i += 1

        soa = np.array(config_vectors)
        x, y, z, u, v, w = zip(*soa)
        config_ax.quiver(x, y, z, u, v, w, pivot='tail', length=0.1)

        config_ax.set_xlim3d(-0.2, 0.2)
        config_ax.set_ylim3d(-0.2, 0.2)
        config_ax.set_zlim3d(-0.2, 0.2)

    def draw_pnp(self, pnp_ax, scat, quiver, points, angles, camera_matrix, dist_coeffs):
        try:
            scat.remove()
            quiver.remove()
        except ValueError:
            pass

        object_points = []
        image_points = []

        pnp_ax.set_xlim3d(-0.75, 0.75)
        pnp_ax.set_zlim3d(-0.75, 0.75)
        pnp_ax.set_ylim3d(-2.5, -1.0)

        for i, a in angles.items():
            image_point = [(a[0] / ticks_per_degree) - angle_range_v[0],
                           (a[1] / ticks_per_degree) - angle_range_h[0]]
            image_points.append(image_point)
            object_points.append(points[i])

        image_points_np = np.array(image_points)
        object_points_np = np.array(object_points)

        print("we have %d image points and %d object points" % (len(image_points), len(object_points)))

        if len(object_points_np) > 3 and len(image_points) == len(object_points):
            # print("image points", image_points_np)
            # print("object points", object_points_np)

            # retval, rvec, tvec = cv2.solvePnP(object_points_np, image_points_np, cameraMatrix, distCoeffs)
            ret, rvec, tvec, inliers = cv2.solvePnPRansac(object_points_np, image_points_np,
                                                          camera_matrix, dist_coeffs)
            if ret:
                print("tvec\n", tvec)
                print("rvec\n", rvec)
                scat = pnp_ax.scatter(tvec[0], -tvec[2], tvec[1], color="blue")

                quiver = pnp_ax.quiver(tvec[0], -tvec[2], tvec[1],
                                       rvec[0], -rvec[2], rvec[1],
                                       pivot='tail', length=0.1)

                return scat, quiver

            else:
                print("No correspondences found!")

    def plot(self):
        config_str = self.vive.get_config().decode()

        points, normals = self.get_light_sensor_positions(config_str)

        #camera_matrix = np.eye(3)

        """
        fx = 1.0
        fy = 1.0
        cx = 0.0
        cy = 0.0

        cameraMatrix = np.array([[fx,0.0,cx],
                                 [0.0,fy,cy],
                                 [0.0,0.0,1.0]])
        """

        fx = 0.5
        w = angle_range_v[1] - angle_range_v[0]
        h = angle_range_h[1] - angle_range_h[0]

        camera_matrix = np.float64([[fx * w, 0.0,    0.5 * (w - 1)],
                                    [0.0,    fx * w, 0.5 * (h - 1)],
                                    [0.0,    0.0,    1.0]])
        dist_coeffs = np.zeros(4)

        matplotlib.interactive(True)

        handler = SigHandler()

        f = plt.figure()

        f.text(0.5, 0.04, 'vive-libre Lighthouse Tracking', ha='center', va='center')

        pnp_ax = f.add_subplot(1, 3, 1, aspect=1, projection='3d')
        angles_ax = f.add_subplot(1, 3, 2, aspect=1)
        config_ax = f.add_subplot(1, 3, 3, aspect=1, projection='3d')

        pnp_ax.set_title('OpenCV solvePnPRansac')
        config_ax.set_title('JSON Config')

        self.draw_config(config_ax, points, normals)

        scat = pnp_ax.scatter(0, 0, 0)
        quiver = pnp_ax.quiver(0, 0, 0, 0, 0, 0)

        while not handler.quit:
            angles = self.vive.poll_angles(b'A', 1000)

            ret = self.draw_pnp(pnp_ax, scat, quiver, points, angles, camera_matrix, dist_coeffs)

            if ret:
                scat, quiver = ret

            self.draw_angles(angles_ax, angles)

            plt.draw()
            plt.pause(0.05)

    def plot_from_cpp(self):
        matplotlib.interactive(True)

        handler = SigHandler()

        f = plt.figure()

        f.text(0.5, 0.04, 'vive-libre Lighthouse Tracking', ha='center', va='center')

        pnp_ax = f.add_subplot(1, 1, 1, aspect=1, projection='3d')

        pnp_ax.set_title('OpenCV solvePnPRansac')

        scat = pnp_ax.scatter(0, 0, 0)
        quiver = pnp_ax.quiver(0, 0, 0, 0, 0, 0)

        while not handler.quit:
            tvec, rvec = self.vive.poll_pnp(b'A', 1000)

            try:
                scat.remove()
                quiver.remove()
            except ValueError:
                pass

            pnp_ax.set_xlim3d(-0.75, 0.75)
            pnp_ax.set_zlim3d(-0.75, 0.75)
            pnp_ax.set_ylim3d(-2.5, -1.0)

            print("tvec py", tvec)
            #print("rvec\n", rvec)
            scat = pnp_ax.scatter(tvec[0], -tvec[2], tvec[1], color="blue")

            quiver = pnp_ax.quiver(tvec[0], -tvec[2], tvec[1],
                                           rvec[0], -rvec[2], rvec[1],
                                           pivot='tail', length=0.1)

            plt.draw()
            plt.pause(0.05)

if __name__ == '__main__':
    vive_pos = VivePos()

    #vive_pos.plot()
    vive_pos.plot_from_cpp()
