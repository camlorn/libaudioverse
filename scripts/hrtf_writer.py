"""This is a helper class for scripts to ipmport from various HRTF sources."""

import numpy
import struct
import enum

EndiannessTypes=enum.Enum("EndiannessTypes", "big little")

class HrtfWriter(object):
	endianness_marker = 1
	#The odd syntax here lets us put comments in.
	format_template="".join([
	"{}", #endianness and size indicator. This is platform-dependent.
	"2i", #Endianness marker, samplerate.
	"4i", #Response count, number of elevations, min elevation, max elevation.
	"{}", #Hole for the azimuth counts.
	"i", #Length of each response in samples.
	"{}", #hole for the responses.
	])

	def __init__(self, samplerate, elevation_count, min_elevation, max_elevation, responses, endianness=EndiannessTypes.little, print_progress=True):
		"""Parameters should all be integers:
		samplerate: obvious.
		elevation_count: Total number of elevations.
		min_elevation: Lowest elevation in degrees.
		max_elevation: Highest elevation in degrees.
		responses: List of lists of Numpy arrays in any format.
		Each sublist is one elevation, and they should be stored in ascending order (lowest elevation first).
		endianness: Endianness of the target CPU.
		print_progress: If true, using this class prints progress information to stdout.
		"""
		self.samplerate=int(samplerate)
		self.elevation_count = int(elevation_count)
		self.min_elevation = int(min_elevation)
		self.max_elevation = int(max_elevation)
		self.responses=responses
		self.endianness = endianness
		self.print_progress=print_progress
		#Some sanity checks.
		if len(responses) !=elevation_count:
			raise ValueError("Not enough elevations; got {} but expected {}".format(len(responses), elevation_count))
		self.azimuth_counts = []
		for i in responses:
			if len(i) ==0:
				raise ValueError("Elevation {} is empty.".format(i))
			self.azimuth_counts.append(i)
		response_lengths = [len(response) for response in elevation for elevation in self.responses]
		for i in response_lengths:
			if i != response_lengths[0]:
				raise valueError("Responses must all have the same length.")
		self.response_length = response_lengths[0]
		self.response_count=sum((len(elev) for elev in self.responses))
		if print_progress:
			print "basic sanity checks passed and HRTF Writer initialized."
			print "Dataset has {} responses and {} elevations".format(self.response_length, self.elevation_count)
			print "sr =", self.samplerate
			print self.elevation_count, "elevations."
			print "Min elevation =", self.min_elevation, "max elevation = ", self.max_elevation
			print "Azimuth counts: {}".format(self.azimuth_counts)

	def make_format_string(self):
		endianness_token= "<" if self.endianness == EndiannessTypes.little else ">"
		self.format_string=self.format_template.format(endianness_token, str(len(self.azimuth_counts))+"i", str(self.response_count*self.response_length)+"f")
		if self.print_progress:
			print "Format string:", self.format_string

	def pack_data():
		iter = itertools.chain(
		[self.endianness_marker, self.samplerate, self.response_count,
		self.elevation_count, self.min_elevation, self.max_elevation],
		self.azimuth_counts,
		[self.response_length],
		[response for response in  elevation for elevation in self.responses])
		data=list(iter)
		self.packed_data = struct.pack(self.format_string, *data)
		if self.print_progress:
			print "Data packed. Total size is {}.".format(len(self.packed_data))

	def write_file(path):
		if not hasattr(self, 'packed_data'):
			raise ValueError("Must pack data first.")
		with file(path, "wb") as f:
			f.write(self.packed_data)
		if self.print_progress:
			print "Data written to {}".format(path)

	def standard_build(path):
		"""Does a standard build, that is the transformations that should be made on most HRIRs."""
		self.make_format_string()
		self.pack_data()
		self.write_file(path)
