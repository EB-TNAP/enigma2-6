from Screens.Screen import Screen
from Screens.MessageBox import MessageBox
from Screens.Standby import getReasons
from Components.Sources.StaticText import StaticText
from Components.ChoiceList import ChoiceList, ChoiceEntryComponent
from Components.config import config, configfile
from Components.ActionMap import ActionMap
from Components.Console import Console
from Components.Label import Label
from Components.Pixmap import Pixmap
from Components.ProgressBar import ProgressBar
from Components.SystemInfo import SystemInfo
from Tools.BoundFunction import boundFunction
from Tools.Directories import resolveFilename, SCOPE_PLUGINS
from Tools.Downloader import downloadWithProgress
from Tools.HardwareInfo import HardwareInfo
from Tools.Multiboot import getImagelist, getCurrentImage, getCurrentImageMode, deleteImage, restoreImages
import os
from urllib.request import urlopen
import json
import time
import zipfile
import shutil
import tempfile
import struct
import hashlib

from enigma import eEPGCache


def checkimagefiles(files):
	return len([x for x in files if 'kernel' in x and '.bin' in x or x in ('uImage', 'rootfs.bin', 'root_cfe_auto.bin', 'root_cfe_auto.jffs2', 'oe_rootfs.bin', 'e2jffs2.img', 'rootfs.tar.bz2', 'rootfs.ubi')]) == 2


class SelectImage(Screen):
	def __init__(self, session, *args):
		Screen.__init__(self, session)
		model = HardwareInfo().get_machine_name()
		self.jsonlist = {}
		print("############ self.jsonlistmodel = ", model)
		self.imagesList = {}
		self.setIndex = 0
		self.expanded = []
		self.setTitle(_("Select image"))
		self["key_red"] = StaticText(_("Cancel"))
		self["key_green"] = StaticText()
		self["key_yellow"] = StaticText()
		self["description"] = StaticText()
		self["list"] = ChoiceList(list=[ChoiceEntryComponent('', ((_("Retrieving image list - Please wait...")), "Waiter"))])

		self["actions"] = ActionMap(["OkCancelActions", "ColorActions", "DirectionActions", "KeyboardInputActions", "MenuActions"],
		{
			"ok": self.keyOk,
			"cancel": boundFunction(self.close, None),
			"red": boundFunction(self.close, None),
			"green": self.keyOk,
			"yellow": self.keyDelete,
			"up": self.keyUp,
			"down": self.keyDown,
			"left": self.keyLeft,
			"right": self.keyRight,
			"upRepeated": self.keyUp,
			"downRepeated": self.keyDown,
			"leftRepeated": self.keyLeft,
			"rightRepeated": self.keyRight,
			"menu": boundFunction(self.close, True),
		}, -1)

		self.callLater(self.getImagesList)

	def getImagesList(self):

		def getImages(path, files):
			for file in [x for x in files if os.path.splitext(x)[1] == ".zip" and model in x]:
				try:
					if checkimagefiles([x.split(os.sep)[-1] for x in zipfile.ZipFile(file).namelist()]):
						imagetyp = _("Downloaded Images")
						if 'backup' in file.split(os.sep)[-1]:
							imagetyp = _("Fullbackup Images")
						if imagetyp not in self.imagesList:
							self.imagesList[imagetyp] = {}
						self.imagesList[imagetyp][file] = {'link': file, 'name': file.split(os.sep)[-1]}
				except:
					pass

		model = HardwareInfo().get_machine_name()

		if not self.imagesList:
			if not self.jsonlist:
				try:
					self.jsonlist = dict(json.load(urlopen('http://162.216.113.217/json-5/%s.json' % model)))
					print("############ self.jsonlist = ", self.jsonlist)
					if config.usage.alternative_imagefeed.value:
						self.jsonlist.update(dict(json.load(urlopen('%s%s' % (config.usage.alternative_imagefeed.value, model)))))
				except:
					pass
			self.imagesList = dict(self.jsonlist)

			for media in ['/media/%s' % x for x in os.listdir('/media')] + (['/media/net/%s' % x for x in os.listdir('/media/net')] if os.path.isdir('/media/net') else []):
				try:
					getImages(media, [os.path.join(media, x) for x in os.listdir(media) if os.path.splitext(x)[1] == ".zip" and model in x])
					for folder in ["images", "downloaded_images", "imagebackups"]:
						if folder in os.listdir(media):
							subfolder = os.path.join(media, folder)
							if os.path.isdir(subfolder) and not os.path.islink(subfolder) and not os.path.ismount(subfolder):
								getImages(subfolder, [os.path.join(subfolder, x) for x in os.listdir(subfolder) if os.path.splitext(x)[1] == ".zip" and model in x])
								for dir in [dir for dir in [os.path.join(subfolder, dir) for dir in os.listdir(subfolder)] if os.path.isdir(dir) and os.path.splitext(dir)[1] == ".unzipped"]:
									shutil.rmtree(dir)
				except:
					pass

		list = []
		for catagorie in reversed(sorted(self.imagesList.keys())):
			if catagorie in self.expanded:
				list.append(ChoiceEntryComponent('expanded', ((str(catagorie)), "Expander")))
				for image in reversed(sorted(self.imagesList[catagorie].keys())):
					list.append(ChoiceEntryComponent('verticalline', ((str(self.imagesList[catagorie][image]['name'])), str(self.imagesList[catagorie][image]['link']))))
			else:
				for image in self.imagesList[catagorie].keys():
					list.append(ChoiceEntryComponent('expandable', ((str(catagorie)), "Expander")))
					break
		if list:
			self["list"].setList(list)
			if self.setIndex:
				self["list"].moveToIndex(self.setIndex if self.setIndex < len(list) else len(list) - 1)
				if self["list"].l.getCurrentSelection()[0][1] == "Expander":
					self.setIndex -= 1
					if self.setIndex:
						self["list"].moveToIndex(self.setIndex if self.setIndex < len(list) else len(list) - 1)
				self.setIndex = 0
			self.selectionChanged()
		else:
			self.session.openWithCallback(self.close, MessageBox, _("Cannot find images - please try later"), type=MessageBox.TYPE_ERROR, timeout=3)

	def keyOk(self):
		currentSelected = self["list"].l.getCurrentSelection()
		if currentSelected[0][1] == "Expander":
			if currentSelected[0][0] in self.expanded:
				self.expanded.remove(currentSelected[0][0])
			else:
				self.expanded.append(currentSelected[0][0])
			self.getImagesList()
		elif currentSelected[0][1] != "Waiter":
			self.session.openWithCallback(self.getImagesList, FlashImage, currentSelected[0][0], currentSelected[0][1])

	def keyDelete(self):
		currentSelected = self["list"].l.getCurrentSelection()[0][1]
		if not("://" in currentSelected or currentSelected in ["Expander", "Waiter"]):
			try:
				os.remove(currentSelected)
				currentSelected = ".".join([currentSelected[:-4], "unzipped"])
				if os.path.isdir(currentSelected):
					shutil.rmtree(currentSelected)
				self.setIndex = self["list"].getSelectedIndex()
				self.imagesList = []
				self.getImagesList()
			except:
				self.session.open(MessageBox, _("Cannot delete downloaded image"), MessageBox.TYPE_ERROR, timeout=3)

	def selectionChanged(self):
		currentSelected = self["list"].l.getCurrentSelection()
		if "://" in currentSelected[0][1] or currentSelected[0][1] in ["Expander", "Waiter"]:
			self["key_yellow"].setText("")
		else:
			self["key_yellow"].setText(_("Delete image"))
		if currentSelected[0][1] == "Waiter":
			self["key_green"].setText("")
		else:
			if currentSelected[0][1] == "Expander":
				self["key_green"].setText(_("Compress") if currentSelected[0][0] in self.expanded else _("Expand"))
				self["description"].setText("")
			else:
				self["key_green"].setText(_("Flash Image"))
				self["description"].setText(currentSelected[0][1])

	def keyLeft(self):
		self["list"].instance.moveSelection(self["list"].instance.pageUp)
		self.selectionChanged()

	def keyRight(self):
		self["list"].instance.moveSelection(self["list"].instance.pageDown)
		self.selectionChanged()

	def keyUp(self):
		self["list"].instance.moveSelection(self["list"].instance.moveUp)
		self.selectionChanged()

	def keyDown(self):
		self["list"].instance.moveSelection(self["list"].instance.moveDown)
		self.selectionChanged()


class FlashImage(Screen):
	skin = """<screen position="center,center" size="640,150" flags="wfNoBorder" backgroundColor="#54242424">
		<widget name="header" position="5,10" size="e-10,50" font="Regular;40" backgroundColor="#54242424"/>
		<widget name="info" position="5,60" size="e-10,130" font="Regular;24" backgroundColor="#54242424"/>
		<widget name="progress" position="5,e-39" size="e-10,24" backgroundColor="#54242424"/>
	</screen>"""

	BACKUP_SCRIPT = resolveFilename(SCOPE_PLUGINS, "Extensions/AutoBackup/settings-backup.sh")

	def __init__(self, session, imagename, source):
		Screen.__init__(self, session)
		self.containerbackup = None
		self.containerofgwrite = None
		self.getImageList = None
		self.downloader = None
		self.source = source
		self.imagename = imagename
		self.reasons = getReasons(session)

		self["header"] = Label(_("Backup settings"))
		self["info"] = Label(_("Save settings and EPG data"))
		self["progress"] = ProgressBar()
		self["progress"].setRange((0, 100))
		self["progress"].setValue(0)

		self["actions"] = ActionMap(["OkCancelActions", "ColorActions"],
		{
			"cancel": self.abort,
			"red": self.abort,
			"ok": self.ok,
			"green": self.ok,
		}, -1)

		self.callLater(self.confirmation)

	def confirmation(self):
		if self.reasons:
			self.message = _("%s\nDo you still want to flash image\n%s?") % (self.reasons, self.imagename)
		else:
			self.message = _("Do you want to flash image\n%s") % self.imagename
		if SystemInfo["canMultiBoot"]:
			imagesList = getImagelist()
			currentimageslot = getCurrentImage()
			choices = []
			slotdict = {k: v for k, v in SystemInfo["canMultiBoot"].items() if not v['device'].startswith('/dev/sda')}
			for x in range(1, len(slotdict) + 1):
				choices.append(((_("slot%s - %s (current image) with, backup") if x == currentimageslot else _("slot%s - %s, with backup")) % (x, imagesList[x]['imagename']), (x, "with backup")))
			for x in range(1, len(slotdict) + 1):
				choices.append(((_("slot%s - %s (current image), without backup") if x == currentimageslot else _("slot%s - %s, without backup")) % (x, imagesList[x]['imagename']), (x, "without backup")))
			choices.append((_("No, do not flash image"), False))
			self.session.openWithCallback(self.checkMedia, MessageBox, self.message, list=choices, default=currentimageslot, simple=True)
		else:
			choices = [(_("Yes, with backup"), "with backup"), (_("Yes, without backup"), "without backup"), (_("No, do not flash image"), False)]
			self.session.openWithCallback(self.checkMedia, MessageBox, self.message, list=choices, default=False, simple=True)

	def checkMedia(self, retval):
		if retval:
			if SystemInfo["canMultiBoot"]:
				self.multibootslot = retval[0]
				doBackup = retval[1] == "with backup"
			else:
				doBackup = retval == "with backup"

			def findmedia(path):
				def avail(path):
					if not path.startswith('/mmc') and os.path.isdir(path) and os.access(path, os.W_OK):
						try:
							statvfs = os.statvfs(path)
							return (statvfs.f_bavail * statvfs.f_frsize) / (1 << 20)
						except:
							pass

				def checkIfDevice(path, diskstats):
					st_dev = os.stat(path).st_dev
					return (os.major(st_dev), os.minor(st_dev)) in diskstats

				diskstats = [(int(x[0]), int(x[1])) for x in [x.split()[0:3] for x in open('/proc/diskstats').readlines()] if x[2].startswith("sd")]
				if os.path.isdir(path) and checkIfDevice(path, diskstats) and avail(path) > 500:
					return (path, True)
				mounts = []
				devices = []
				for path in ['/media/%s' % x for x in os.listdir('/media')] + (['/media/net/%s' % x for x in os.listdir('/media/net')] if os.path.isdir('/media/net') else []):
					try:
						if checkIfDevice(path, diskstats):
							devices.append((path, avail(path)))
						else:
							mounts.append((path, avail(path)))
					except OSError:
						pass
				devices.sort(key=lambda x: x[1], reverse=True)
				mounts.sort(key=lambda x: x[1], reverse=True)
				return ((devices[0][1] > 500 and (devices[0][0], True)) if devices else mounts and mounts[0][1] > 500 and (mounts[0][0], False)) or (None, None)

			self.destination, isDevice = findmedia(os.path.isfile(self.BACKUP_SCRIPT) and hasattr(config.plugins, "autobackup") and config.plugins.autobackup.where.value or "/media/hdd")

			if self.destination:

				destination = os.path.join(self.destination, 'downloaded_images')
				self.zippedimage = "://" in self.source and os.path.join(destination, self.imagename) or self.source
				self.unzippedimage = os.path.join(destination, '%s.unzipped' % self.imagename[:-4])

				try:
					if os.path.isfile(destination):
						os.remove(destination)
					if not os.path.isdir(destination):
						os.mkdir(destination)
					if doBackup:
						if isDevice:
							self.startBackupsettings(True)
						else:
							self.session.openWithCallback(self.startBackupsettings, MessageBox, _("Can only find a network drive to store the backup this means after the flash the autorestore will not work. Alternativaly you can mount the network drive after the flash and perform a manufacurer reset to autorestore"), simple=True)
					else:
						self.startDownload()
				except:
					self.session.openWithCallback(self.abort, MessageBox, _("Unable to create the required directories on the media (e.g. USB stick or Harddisk) - Please verify media and try again!"), type=MessageBox.TYPE_ERROR, simple=True)
			else:
				self.session.openWithCallback(self.abort, MessageBox, _("Could not find suitable media - Please remove some downloaded images or insert a media (e.g. USB stick) with sufficiant free space and try again!"), type=MessageBox.TYPE_ERROR, simple=True)
		else:
			self.abort()

	def startBackupsettings(self, retval):
		if retval:
			if os.path.isfile(self.BACKUP_SCRIPT):
				self["info"].setText(_("Backing up to: %s") % self.destination)
				configfile.save()
				if config.plugins.autobackup.epgcache.value:
					eEPGCache.getInstance().save()
				self.containerbackup = Console()
				self.containerbackup.ePopen("%s%s'%s' %s" % (self.BACKUP_SCRIPT, config.plugins.autobackup.autoinstall.value and " -a " or " ", self.destination, int(config.plugins.autobackup.prevbackup.value)), self.backupsettingsDone)
			else:
				self.session.openWithCallback(self.startDownload, MessageBox, _("Unable to backup settings as the AutoBackup plugin is missing, do you want to continue?"), default=False, simple=True)
		else:
			self.abort()

	def backupsettingsDone(self, data, retval, extra_args):
		self.containerbackup = None
		if retval == 0:
			self.startDownload()
		else:
			self.session.openWithCallback(self.abort, MessageBox, _("Error during backup settings\n%s") % retval, type=MessageBox.TYPE_ERROR, simple=True)

	def startDownload(self, reply=True):
		self.show()
		if reply:
			if "://" in self.source:
				from Tools.Downloader import downloadWithProgress
				self["header"].setText(_("Downloading Image"))
				self["info"].setText(self.imagename)
				self.downloader = downloadWithProgress(self.source, self.zippedimage)
				self.downloader.addProgress(self.downloadProgress)
				self.downloader.addEnd(self.downloadEnd)
				self.downloader.addError(self.downloadError)
				self.downloader.start()
			else:
				self.unzip()
		else:
			self.abort()

	def downloadProgress(self, current, total):
		self["progress"].setValue(int(100 * current / total))

	def downloadError(self, reason, status):
		self.downloader.stop()
		self.session.openWithCallback(self.abort, MessageBox, _("Error during downloading image\n%s\n%s") % (self.imagename, reason), type=MessageBox.TYPE_ERROR, simple=True)

	def downloadEnd(self):
		self.downloader.stop()
		self.verifyChecksum()

	def verifyChecksum(self):
		self["header"].setText(_("Verifying Image Integrity"))
		self["info"].setText("%s\n%s" % (self.imagename, _("Checking file integrity...")))
		self["progress"].setValue(0)
		self.callLater(self.doVerifyChecksum)

	def doVerifyChecksum(self):
		try:
			# Check if this is a TNAP image from tnapimages.com or TNAP IP server (162.216.113.217)
			source_str = str(self.source).lower()
			is_tnap_source = ("tnapimages.com" in source_str or "162.216.113.217" in source_str)
			is_tnap_image = ("tnap" in str(self.imagename).lower())
			
			print("[FlashImage] TNAP source: %s, TNAP image: %s, Source: %s" % (is_tnap_source, is_tnap_image, source_str))
			
			if is_tnap_source and is_tnap_image:
				self["info"].setText("%s\n%s" % (self.imagename, _("Fetching checksums from tnapimages.com...")))
				self["progress"].setValue(25)
				
				# Fetch checksums from the enhanced API I built
				checksum_data = self.fetchTNAPChecksums()
				if checksum_data:
					self["info"].setText("%s\n%s" % (self.imagename, _("Calculating SHA256 hash...")))
					self["progress"].setValue(50)
					
					# Calculate file hash
					file_hash = self.calculateSHA256(self.zippedimage)
					self["progress"].setValue(75)
					
					# Verify against expected hash
					expected_sha256 = str(checksum_data['sha256']) if checksum_data['sha256'] else ''
					if file_hash.lower() == expected_sha256.lower():
						self["header"].setText(_("✓ Checksum Verification Successful"))
						self["info"].setText("%s\n%s\nSHA256: %s\n\n%s" % (self.imagename, _("File integrity verified!"), file_hash[:16] + "...", _("Proceeding with installation in 3 seconds...")))
						self["progress"].setValue(100)
						from enigma import eTimer
						self.verification_timer = eTimer()
						self.verification_timer.callback.append(self.unzip)
						self.verification_timer.start(3000, True)  # 3 second delay
					else:
						self.session.openWithCallback(self.checksumFailed, MessageBox, 
							_("⚠️ Checksum Verification Failed!\n\nExpected: %s\nActual: %s\n\nThis may indicate file corruption or tampering.\nDo you want to continue anyway?") % 
							(expected_sha256[:16] + "...", file_hash[:16] + "..."), 
							type=MessageBox.TYPE_YESNO, default=False)
				else:
					# No checksum available, proceed with warning
					self.session.openWithCallback(self.noChecksumWarning, MessageBox, 
						_("⚠️ No Checksum Available\n\nUnable to verify file integrity for this image.\nThis may be normal for non-TNAP images.\nDo you want to continue?"), 
						type=MessageBox.TYPE_YESNO, default=True)
			else:
				# Not a TNAP image, skip verification
				self.unzip()
		except Exception as e:
			# Verification failed, offer to continue
			self.session.openWithCallback(self.verificationError, MessageBox, 
				_("Verification Error: %s\n\nDo you want to continue without verification?") % str(e), 
				type=MessageBox.TYPE_YESNO, default=False)

	def fetchTNAPChecksums(self):
		try:
			# Use the enhanced API endpoint I built
			api_url = "https://tnapimages.com/downloads/list-tnap-files-enhanced.php"
			print("[FlashImage] Fetching checksums from:", api_url)
			response = urlopen(api_url)
			data = json.load(response)
			
			print("[FlashImage] API returned type:", type(data))
			print("[FlashImage] API data preview:", str(data)[:200])
			
			# Handle different response formats
			print("[FlashImage] Analyzing response structure...")
			print("[FlashImage] All keys in response:", list(data.keys()) if isinstance(data, dict) else "Not a dict")
			
			if isinstance(data, dict):
				# Check for various possible data structures
				if 'files' in data:
					print("[FlashImage] Found 'files' key")
					files_data = data['files']
				elif 'file_list' in data:
					print("[FlashImage] Found 'file_list' key") 
					files_data = data['file_list']  
				elif 'checksums' in data:
					print("[FlashImage] Found 'checksums' key")
					files_data = data['checksums']
				elif 'api_version' in data:
					print("[FlashImage] Found 'api_version' key - metadata response detected")
					# This looks like a metadata response, find the actual files
					found_files = False
					for key in data:
						if isinstance(data[key], list) and key != 'download_stats':
							files_data = data[key]
							print("[FlashImage] Found files in key:", key, "with", len(files_data), "items")
							found_files = True
							break
					if not found_files:
						print("[FlashImage] API returned metadata but no files found")
						print("[FlashImage] Available keys:", list(data.keys()))
						# Show a sample of each key's type and value
						for key, value in data.items():
							print("[FlashImage] Key '%s': type=%s, value_preview=%s" % (key, type(value), str(value)[:100]))
						return None
				else:
					print("[FlashImage] No standard keys found, treating as single file response")
					files_data = [data]  # Single file response
			elif isinstance(data, list):
				files_data = data
			else:
				print("[FlashImage] Unexpected API response format:", type(data))
				return None
			
			# Find checksum for our specific file
			filename = os.path.basename(self.zippedimage)
			print("[FlashImage] Looking for file:", filename)
			print("[FlashImage] Files data type:", type(files_data))
			print("[FlashImage] Files data length:", len(files_data) if isinstance(files_data, (list, dict)) else "N/A")
			
			# Show a sample of what's in files_data
			if isinstance(files_data, list):
				print("[FlashImage] First few files in array:")
				for i, item in enumerate(files_data[:3]):  # Show first 3 items
					if isinstance(item, dict):
						item_name = item.get('name', item.get('filename', 'UNKNOWN'))
						print("[FlashImage]   [%d] name='%s', keys=%s" % (i, item_name, list(item.keys())))
					else:
						print("[FlashImage]   [%d] type=%s, value=%s" % (i, type(item), str(item)[:50]))
			elif isinstance(files_data, dict):
				print("[FlashImage] Files data is a dict with keys:", list(files_data.keys()))
				# Files are organized by receiver model, find our model
				model_key = None
				for key in files_data.keys():
					if key in filename.lower() or key in self.source.lower():
						model_key = key
						break
				
				if not model_key:
					# Try common model detection patterns
					if 'sf8008' in filename.lower() or 'sf8008' in self.source.lower():
						model_key = 'sf8008'
					elif 'osmio4k' in filename.lower() or 'osmio4k' in self.source.lower():
						model_key = 'osmio4k' 
					elif 'osmini4k' in filename.lower() or 'osmini4k' in self.source.lower():
						model_key = 'osmini4k'
					elif 'ustym4kpro' in filename.lower() or 'ustym4kpro' in self.source.lower():
						model_key = 'ustym4kpro'
				
				if model_key and model_key in files_data:
					print("[FlashImage] Using model key:", model_key)
					files_data = files_data[model_key]
					print("[FlashImage] Model files count:", len(files_data))
				else:
					print("[FlashImage] Could not determine model or model not found")
					print("[FlashImage] Available models:", list(files_data.keys()))
					return None
			
			# Now search through the files
			for file_info in files_data:
				if isinstance(file_info, dict):
					file_name = file_info.get('name', file_info.get('filename', ''))
					print("[FlashImage] Checking file:", file_name, "against", filename)
					
					if file_name == filename:
						print("[FlashImage] File info keys:", list(file_info.keys()))
						print("[FlashImage] Has checksum field:", 'checksum' in file_info)
						
						if 'checksum' in file_info or 'checksum_sha256' in file_info or 'sha256' in file_info:
							print("[FlashImage] Found checksum for:", filename)
							
							# Try multiple possible hash field names
							sha256_hash = (file_info.get('checksum_sha256') or 
										  file_info.get('sha256') or 
										  file_info.get('checksum') or 
										  file_info.get('hash_sha256'))
							
							# Handle nested checksum structures
							checksum_data = file_info.get('checksum')
							if isinstance(checksum_data, dict):
								sha256_hash = checksum_data.get('sha256') or checksum_data.get('checksum_sha256')
							elif isinstance(checksum_data, str):
								sha256_hash = checksum_data
							
							print("[FlashImage] Extracted hash:", sha256_hash[:16] + "..." if sha256_hash else "None")
							
							return {
								'sha256': str(sha256_hash) if sha256_hash else '',
								'md5': file_info.get('checksum_md5', file_info.get('md5', '')),
								'size': file_info.get('size_bytes', file_info.get('size', 0))
							}
			
			print("[FlashImage] No checksum found for:", filename)
			print("[FlashImage] Total files checked:", len(files_data) if isinstance(files_data, (list, dict)) else "N/A")
			return None
		except Exception as e:
			print("[FlashImage] Checksum fetch error:", e)
			import traceback
			traceback.print_exc()
			return None

	def calculateSHA256(self, filepath):
		sha256_hash = hashlib.sha256()
		with open(filepath, "rb") as f:
			for chunk in iter(lambda: f.read(8192), b""):
				sha256_hash.update(chunk)
		return sha256_hash.hexdigest()

	def checksumFailed(self, result):
		if result:
			# User chose to continue despite checksum failure
			self.unzip()
		else:
			# User chose to abort
			self.session.openWithCallback(self.abort, MessageBox, 
				_("Flash operation cancelled due to checksum verification failure."), 
				type=MessageBox.TYPE_ERROR, timeout=5)

	def noChecksumWarning(self, result):
		if result:
			self.unzip()
		else:
			self.abort()

	def verificationError(self, result):
		if result:
			self.unzip()
		else:
			self.abort()

	def unzip(self):
		self["header"].setText(_("Unzipping Image"))
		self["info"].setText("%s\n%s" % (self.imagename, _("Please wait")))
		self["progress"].hide()
		self.callLater(self.doUnzip)

	def doUnzip(self):
		try:
			zipfile.ZipFile(self.zippedimage, 'r').extractall(self.unzippedimage)
			self.flashimage()
		except:
			self.session.openWithCallback(self.abort, MessageBox, _("Error during unzipping image\n%s") % self.imagename, type=MessageBox.TYPE_ERROR, simple=True)

	def flashimage(self):
		self["header"].setText(_("Flashing Image"))

		def findimagefiles(path):
			for path, subdirs, files in os.walk(path):
				if not subdirs and files:
					return checkimagefiles(files) and path
		imagefiles = findimagefiles(self.unzippedimage)
		if imagefiles:
			if SystemInfo["canMultiBoot"]:
				command = "/usr/bin/ofgwrite -k -r -m%s '%s'" % (self.multibootslot, imagefiles)
			else:
				command = "/usr/bin/ofgwrite -k -r '%s'" % imagefiles
			self.containerofgwrite = Console()
			self.containerofgwrite.ePopen(command, self.FlashimageDone)
		else:
			self.session.openWithCallback(self.abort, MessageBox, _("Image to install is invalid\n%s") % self.imagename, type=MessageBox.TYPE_ERROR, simple=True)

	def FlashimageDone(self, data, retval, extra_args):
		self.containerofgwrite = None
		if retval == 0:
			self["header"].setText(_("Flashing image successful"))
			self["info"].setText(_("%s\nPress ok for multiboot selection\nPress exit to close") % self.imagename)
		else:
			self.session.openWithCallback(self.abort, MessageBox, _("Flashing image was not successful\n%s") % self.imagename, type=MessageBox.TYPE_ERROR, simple=True)

	def abort(self, reply=None):
		if self.getImageList or self.containerofgwrite:
			return 0
		if self.downloader:
			self.downloader.stop()
		if self.containerbackup:
			self.containerbackup.killAll()
		self.close()

	def ok(self):
		if self["header"].text == _("Flashing image successful"):
			self.session.openWithCallback(self.abort, MultibootSelection)
		else:
			return 0


class MultibootSelection(SelectImage):
	def __init__(self, session, *args):
		SelectImage.__init__(self, session)
		self.skinName = ["MultibootSelection", "SelectImage"]
		self.expanded = []
		self.tmp_dir = None
		self.setTitle(_("Multiboot image selector"))
		self["key_red"] = StaticText(_("Cancel"))
		self["key_green"] = StaticText(_("Reboot"))
		self["key_yellow"] = StaticText()
		self["list"] = ChoiceList([])

		self["actions"] = ActionMap(["OkCancelActions", "ColorActions", "DirectionActions", "KeyboardInputActions", "MenuActions"],
		{
			"ok": self.keyOk,
			"cancel": self.cancel,
			"red": self.cancel,
			"green": self.keyOk,
			"yellow": self.deleteImage,
			"up": self.keyUp,
			"down": self.keyDown,
			"left": self.keyLeft,
			"right": self.keyRight,
			"upRepeated": self.keyUp,
			"downRepeated": self.keyDown,
			"leftRepeated": self.keyLeft,
			"rightRepeated": self.keyRight,
			"menu": boundFunction(self.cancel, True),
		}, -1)

		self.currentimageslot = getCurrentImage()
		self.tmp_dir = tempfile.mkdtemp(prefix="MultibootSelection")
		Console().ePopen('mount %s %s' % (SystemInfo["MultibootStartupDevice"], self.tmp_dir))
		self.getImagesList()

	def cancel(self, value=None):
		Console().ePopen('umount %s' % self.tmp_dir)
		if not os.path.ismount(self.tmp_dir):
			os.rmdir(self.tmp_dir)
		if value == 2:
			from Screens.Standby import TryQuitMainloop
			self.session.open(TryQuitMainloop, 2)
		else:
			self.close(value)

	def getImagesList(self):
		list = []
		imagesList = getImagelist()
		mode = getCurrentImageMode() or 0
		self.deletedImagesExists = False
		if imagesList:
			for index, x in enumerate(sorted(imagesList.keys())):
				if imagesList[x]["imagename"] == _("Deleted image"):
					self.deletedImagesExists = True
				elif imagesList[x]["imagename"] != _("Empty slot"):
					if SystemInfo["canMode12"]:
						list.insert(index, ChoiceEntryComponent('', ((_("slot%s - %s mode 1 (current image)") if x == self.currentimageslot and mode != 12 else _("slot%s - %s mode 1")) % (x, imagesList[x]['imagename']), (x, 1))))
						list.append(ChoiceEntryComponent('', ((_("slot%s - %s mode 12 (current image)") if x == self.currentimageslot and mode == 12 else _("slot%s - %s mode 12")) % (x, imagesList[x]['imagename']), (x, 12))))
					else:
						list.append(ChoiceEntryComponent('', ((_("slot%s - %s (current image)") if x == self.currentimageslot and mode != 12 else _("slot%s - %s")) % (x, imagesList[x]['imagename']), (x, 1))))
		if os.path.isfile(os.path.join(self.tmp_dir, "STARTUP_RECOVERY")):
			list.append(ChoiceEntryComponent('', ((_("Boot to Recovery menu")), "Recovery")))
		if os.path.isfile(os.path.join(self.tmp_dir, "STARTUP_ANDROID")):
			list.append(ChoiceEntryComponent('', ((_("Boot to Android image")), "Android")))
		if not list:
			list.append(ChoiceEntryComponent('', ((_("No images found")), "Waiter")))
		self["list"].setList(list)
		self.selectionChanged()

	def deleteImage(self):
		if self["key_yellow"].text == _("Restore deleted images"):
			self.session.openWithCallback(self.deleteImageCallback, MessageBox, _("Are you sure to restore all deleted images"), simple=True)
		elif self["key_yellow"].text == _("Delete Image"):
			self.session.openWithCallback(self.deleteImageCallback, MessageBox, "%s:\n%s" % (_("Are you sure to delete image:"), self.currentSelected[0][0]), simple=True)

	def deleteImageCallback(self, answer):
		if answer:
			if self["key_yellow"].text == _("Restore deleted images"):
				restoreImages()
			else:
				deleteImage(self.currentSelected[0][1][0])
			self.getImagesList()

	def keyOk(self):
		self.session.openWithCallback(self.doReboot, MessageBox, "%s:\n%s" % (_("Are you sure to reboot to"), self.currentSelected[0][0]), simple=True)

	def doReboot(self, answer):
		if answer:
			slot = self.currentSelected[0][1]
			if slot == "Recovery":
				shutil.copyfile(os.path.join(self.tmp_dir, "STARTUP_RECOVERY"), os.path.join(self.tmp_dir, "STARTUP"))
			elif slot == "Android":
				shutil.copyfile(os.path.join(self.tmp_dir, "STARTUP_ANDROID"), os.path.join(self.tmp_dir, "STARTUP"))
			elif SystemInfo["canMultiBoot"][slot[0]]['startupfile']:
				if SystemInfo["canMode12"]:
					startupfile = os.path.join(self.tmp_dir, "%s_%s" % (SystemInfo["canMultiBoot"][slot[0]]['startupfile'].rsplit('_', 1)[0], slot[1]))
				else:
					startupfile = os.path.join(self.tmp_dir, "%s" % SystemInfo["canMultiBoot"][slot[0]]['startupfile'])
				if SystemInfo["canDualBoot"]:
					with open('/dev/block/by-name/flag', 'wb') as f:
						f.write(struct.pack("B", int(slot[0])))
					startupfile = os.path.join("/boot", "%s" % SystemInfo["canMultiBoot"][slot[0]]['startupfile'])
					shutil.copyfile(startupfile, os.path.join("/boot", "STARTUP"))
				else:
					shutil.copyfile(startupfile, os.path.join(self.tmp_dir, "STARTUP"))
			else:
				model = HardwareInfo().get_machine_name()
				if slot[1] == 1:
					startupFileContents = "boot emmcflash0.kernel%s 'root=/dev/mmcblk0p%s rw rootwait %s_4.boxmode=1'\n" % (slot[0], slot[0] * 2 + 1, model)
				else:
					startupFileContents = "boot emmcflash0.kernel%s 'brcm_cma=520M@248M brcm_cma=%s@768M root=/dev/mmcblk0p%s rw rootwait %s_4.boxmode=12'\n" % (slot[0], SystemInfo["canMode12"], slot[0] * 2 + 1, model)
				open(os.path.join(self.tmp_dir, "STARTUP"), 'w').write(startupFileContents)
			self.cancel(2)

	def selectionChanged(self):
		self.currentSelected = self["list"].l.getCurrentSelection()
		if isinstance(self.currentSelected[0][1], tuple) and self.currentimageslot != self.currentSelected[0][1][0]:
			self["key_yellow"].setText(_("Delete Image"))
		elif self.deletedImagesExists:
			self["key_yellow"].setText(_("Restore deleted images"))
		else:
			self["key_yellow"].setText("")
