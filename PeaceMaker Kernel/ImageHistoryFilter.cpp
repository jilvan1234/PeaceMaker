#include "ImageHistoryFilter.h"

StackWalker ImageHistoryFilter::walker;
PPROCESS_HISTORY_ENTRY ImageHistoryFilter::ProcessHistoryHead;
EX_PUSH_LOCK ImageHistoryFilter::ProcessHistoryLock;
BOOLEAN ImageHistoryFilter::destroying;

/**
	Register the necessary notify routines.
	@param InitializeStatus - Status of initialization.
*/
ImageHistoryFilter::ImageHistoryFilter (
	_Out_ NTSTATUS* InitializeStatus
	)
{
	//
	// Set the create process notify routine.
	//
	*InitializeStatus = PsSetCreateProcessNotifyRoutine(ImageHistoryFilter::CreateProcessNotifyRoutine, FALSE);
	if (NT_SUCCESS(*InitializeStatus) == FALSE)
	{
		DBGPRINT("ImageHistoryFilter!ImageHistoryFilter: Failed to register create process notify routine with status 0x%X.", *InitializeStatus);
		return;
	}
	
	//
	// Set the load image notify routine.
	//
	*InitializeStatus = PsSetLoadImageNotifyRoutine(ImageHistoryFilter::LoadImageNotifyRoutine);
	if (NT_SUCCESS(*InitializeStatus) == FALSE)
	{
		DBGPRINT("ImageHistoryFilter!ImageHistoryFilter: Failed to register load image notify routine with status 0x%X.", *InitializeStatus);
		return;
	}

	FltInitializePushLock(&ImageHistoryFilter::ProcessHistoryLock);
}

/**
	Clean up the process history linked-list.
*/
ImageHistoryFilter::~ImageHistoryFilter (
	VOID
	)
{
	PPROCESS_HISTORY_ENTRY currentProcessHistory;

	//
	// Set destroying to TRUE so that no other threads can get a lock.
	//
	ImageHistoryFilter::destroying = TRUE;

	//
	// Remove the notify routines.
	//
	PsSetCreateProcessNotifyRoutine(ImageHistoryFilter::CreateProcessNotifyRoutine, TRUE);
	PsRemoveLoadImageNotifyRoutine(ImageHistoryFilter::LoadImageNotifyRoutine);

	//
	// Acquire an exclusive lock to push out other threads.
	//
	FltAcquirePushLockExclusive(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Release the lock.
	//
	FltReleasePushLock(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Delete the lock for the process history linked-list.
	//
	FltDeletePushLock(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Go through each process history and free it.
	//
	if (ImageHistoryFilter::ProcessHistoryHead)
	{
		while (IsListEmpty(RCAST<PLIST_ENTRY>(ImageHistoryFilter::ProcessHistoryHead)) == FALSE)
		{
			currentProcessHistory = RCAST<PPROCESS_HISTORY_ENTRY>(RemoveHeadList(RCAST<PLIST_ENTRY>(ImageHistoryFilter::ProcessHistoryHead)));



			//
			// Free the stack history.
			//
			ExFreePoolWithTag(RCAST<PVOID>(currentProcessHistory->CallerStackHistory), STACK_HISTORY_TAG);

			//
			// Free the process history.
			//
			ExFreePoolWithTag(SCAST<PVOID>(currentProcessHistory), PROCESS_HISTORY_TAG);
		}

		//
		// Finally, free the list head.
		//
		ExFreePoolWithTag(SCAST<PVOID>(ImageHistoryFilter::ProcessHistoryHead), PROCESS_HISTORY_TAG);
	}
}

/**
	Add a process to the linked-list of process history objects. This function attempts to add a history object regardless of failures.
	@param ProcessId - The process ID of the process to add.
	@param ParentId - The parent process ID of the process to add.
*/
VOID
ImageHistoryFilter::AddProcessToHistory (
	_In_ HANDLE ProcessId,
	_In_ HANDLE ParentId
	)
{
	NTSTATUS status;
	PPROCESS_HISTORY_ENTRY newProcessHistory;
	LARGE_INTEGER systemTime;
	LARGE_INTEGER localSystemTime;
	STACK_RETURN_INFO tempStackReturns[MAX_STACK_RETURN_HISTORY];
	BOOLEAN processHistoryLockHeld;

	processHistoryLockHeld = FALSE;
	status = STATUS_SUCCESS;

	if (ImageHistoryFilter::destroying)
	{
		return;
	}

	newProcessHistory = RCAST<PPROCESS_HISTORY_ENTRY>(ExAllocatePoolWithTag(PagedPool, sizeof(PROCESS_HISTORY_ENTRY), PROCESS_HISTORY_TAG));
	if (newProcessHistory == NULL)
	{
		DBGPRINT("ImageHistoryFilter!AddProcessToHistory: Failed to allocate space for the process history.");
		status = STATUS_NO_MEMORY;
		goto Exit;
	}

	//
	// Basic fields.
	//
	newProcessHistory->ProcessId = ProcessId;
	newProcessHistory->ParentId = ParentId;
	newProcessHistory->CallerId = PsGetCurrentProcessId();
	newProcessHistory->ProcessTerminated = FALSE;
	KeQuerySystemTime(&systemTime);
	ExSystemTimeToLocalTime(&systemTime, &localSystemTime);
	NT_ASSERT(RtlTimeToSecondsSince1970(&localSystemTime, &newProcessHistory->EpochExecutionTime)); // Who is using this garbage after 2105??

	//
	// Image file name fields.
	//

	//
	// The new process name is a requirement.
	//
	if (ImageHistoryFilter::GetProcessImageFileName(ProcessId, &newProcessHistory->ProcessImageFileName) == FALSE)
	{
		DBGPRINT("ImageHistoryFilter!AddProcessToHistory: Failed to get the name of the new process.");
		status = STATUS_NOT_FOUND;
		goto Exit;
	}

	//
	// These fields are optional.
	//
	ImageHistoryFilter::GetProcessImageFileName(ParentId, &newProcessHistory->ParentImageFileName);
	if (PsGetCurrentProcessId() != ParentId)
	{
		ImageHistoryFilter::GetProcessImageFileName(PsGetCurrentProcessId(), &newProcessHistory->CallerImageFileName);
	}

	//
	// Grab the user-mode stack.
	//
	newProcessHistory->CallerStackHistorySize = walker.WalkAndResolveStack(tempStackReturns, MAX_STACK_RETURN_HISTORY);
	newProcessHistory->CallerStackHistory = RCAST<PSTACK_RETURN_INFO>(ExAllocatePoolWithTag(PagedPool, sizeof(STACK_RETURN_INFO) * newProcessHistory->CallerStackHistorySize, STACK_HISTORY_TAG));
	if (newProcessHistory->CallerStackHistory == NULL)
	{
		DBGPRINT("ImageHistoryFilter!AddProcessToHistory: Failed to allocate space for the stack history.");
		status = STATUS_NO_MEMORY;
		goto Exit;
	}
	memcpy(newProcessHistory->CallerStackHistory, tempStackReturns, sizeof(STACK_RETURN_INFO) * newProcessHistory->CallerStackHistorySize);

	//
	// Initialize this last so we don't have to delete it if anything failed.
	//
	FltInitializePushLock(&newProcessHistory->ImageLoadHistoryLock);

	//
	// Grab a lock to add an entry.
	//
	FltAcquirePushLockExclusive(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Check if the list has been initialized.
	//
	if (ImageHistoryFilter::ProcessHistoryHead == NULL)
	{
		ImageHistoryFilter::ProcessHistoryHead = newProcessHistory;
		InitializeListHead(RCAST<PLIST_ENTRY>(ImageHistoryFilter::ProcessHistoryHead));
	}
	//
	// Otherwise, just append the element to the end of the list.
	//
	else
	{
		InsertTailList(RCAST<PLIST_ENTRY>(ImageHistoryFilter::ProcessHistoryHead), RCAST<PLIST_ENTRY>(newProcessHistory));
	}

	FltReleasePushLock(&ImageHistoryFilter::ProcessHistoryLock);
Exit:
	if (newProcessHistory && NT_SUCCESS(status) == FALSE)
	{
		ExFreePoolWithTag(newProcessHistory, PROCESS_HISTORY_TAG);
	}
}

/**
	Set a process to terminated, still maintain the history.
	@param ProcessId - The process ID of the process being terminated.
*/
VOID
ImageHistoryFilter::TerminateProcessInHistory (
	_In_ HANDLE ProcessId
	)
{
	PPROCESS_HISTORY_ENTRY currentProcessHistory;

	if (ImageHistoryFilter::destroying)
	{
		return;
	}

	//
	// Acquire a shared lock to iterate processes.
	//
	FltAcquirePushLockShared(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Iterate histories for a match.
	//
	if (ImageHistoryFilter::ProcessHistoryHead)
	{
		currentProcessHistory = ImageHistoryFilter::ProcessHistoryHead;
		do
		{
			//
			// Find the process history with the same PID and then set it to terminated.
			//
			if (currentProcessHistory->ProcessId == ProcessId)
			{
				currentProcessHistory->ProcessTerminated = TRUE;
				break;
			}
			currentProcessHistory = RCAST<PPROCESS_HISTORY_ENTRY>(currentProcessHistory->ListEntry.Blink);
		} while (currentProcessHistory && currentProcessHistory != ImageHistoryFilter::ProcessHistoryHead);
	}

	//
	// Release the lock.
	//
	FltReleasePushLock(&ImageHistoryFilter::ProcessHistoryLock);
}

/**
	Notify routine called on new process execution.
	@param ParentId - The parent's process ID.
	@param ProcessId - The new child's process ID.
	@param Create - Whether or not this process is being created or terminated.
*/
VOID
ImageHistoryFilter::CreateProcessNotifyRoutine (
	_In_ HANDLE ParentId,
	_In_ HANDLE ProcessId,
	_In_ BOOLEAN Create
	)
{
	//
	// If a new process is being created, add it to the history of processes.
	//
	if (Create)
	{
		ImageHistoryFilter::AddProcessToHistory(ProcessId, ParentId);
	}
	else
	{
		//
		// Set the process as "terminated".
		//
		ImageHistoryFilter::TerminateProcessInHistory(ProcessId);
	}
}

/**
	Retrieve the full image file name for a process.
	@param ProcessId - The process to get the name of.
	@param ProcessImageFileName - PUNICODE_STRING to fill with the image file name of the process.
*/
BOOLEAN
ImageHistoryFilter::GetProcessImageFileName (
	_In_ HANDLE ProcessId,
	_Inout_ PUNICODE_STRING* ImageFileName
	)
{
	NTSTATUS status;
	PEPROCESS processObject;
	HANDLE processHandle;
	ULONG returnLength;

	processHandle = NULL;
	*ImageFileName = NULL;
	returnLength = 0;

	//
	// Before we can open a handle to the process, we need its PEPROCESS object.
	//
	status = PsLookupProcessByProcessId(ProcessId, &processObject);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("ImageHistoryFilter!GetProcessImageFileName: Failed to find process object with status 0x%X.", status);
		goto Exit;
	}

	//
	// Open a handle to the process.
	//
	status = ObOpenObjectByPointer(processObject, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, GENERIC_ALL, *PsProcessType, KernelMode, &processHandle);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("ImageHistoryFilter!GetProcessImageFileName: Failed to open handle to process with status 0x%X.", status);
		goto Exit;
	}

	//
	// Query for the size of the UNICODE_STRING.
	//
	status = NtQueryInformationProcess(processHandle, ProcessImageFileName, NULL, 0, &returnLength);
	if (status != STATUS_INFO_LENGTH_MISMATCH && status != STATUS_BUFFER_TOO_SMALL && status != STATUS_BUFFER_OVERFLOW)
	{
		DBGPRINT("ImageHistoryFilter!GetProcessImageFileName: Failed to query size of process ImageFileName with status 0x%X.", status);
		goto Exit;
	}

	//
	// Allocate the necessary space.
	//
	*ImageFileName = RCAST<PUNICODE_STRING>(ExAllocatePoolWithTag(PagedPool, returnLength, IMAGE_NAME_TAG));
	if (*ImageFileName == NULL)
	{
		DBGPRINT("ImageHistoryFilter!GetProcessImageFileName: Failed to allocate space for process ImageFileName.");
		goto Exit;
	}

	//
	// Query the image file name.
	//
	status = NtQueryInformationProcess(processHandle, ProcessImageFileName, NULL, 0, &returnLength);
	if (NT_SUCCESS(status) == FALSE)
	{
		DBGPRINT("ImageHistoryFilter!GetProcessImageFileName: Failed to query process ImageFileName with status 0x%X.", status);
		goto Exit;
	}
Exit:
	if (processHandle)
	{
		ZwClose(processHandle);
	}
	if (NT_SUCCESS(status) == FALSE && *ImageFileName)
	{
		ExFreePoolWithTag(*ImageFileName, IMAGE_NAME_TAG);
		*ImageFileName = NULL;
	}
	return NT_SUCCESS(status);
}

/**
	Notify routine called when a new image is loaded into a process. Adds the image to the corresponding process history element.
	@param FullImageName - A PUNICODE_STRING that identifies the executable image file. Might be NULL.
	@param ProcessId - The process ID where this image is being mapped.
	@param ImageInfo - Structure containing a variety of properties about the image being loaded.
*/
VOID
ImageHistoryFilter::LoadImageNotifyRoutine(
	_In_ PUNICODE_STRING FullImageName,
	_In_ HANDLE ProcessId,
	_In_ PIMAGE_INFO ImageInfo
	)
{
	NTSTATUS status;
	PPROCESS_HISTORY_ENTRY currentProcessHistory;
	PIMAGE_LOAD_HISTORY_ENTRY newImageLoadHistory;
	STACK_RETURN_INFO tempStackReturns[MAX_STACK_RETURN_HISTORY];

	UNREFERENCED_PARAMETER(ImageInfo);

	currentProcessHistory = NULL;
	newImageLoadHistory = NULL;
	status = STATUS_SUCCESS;

	if (ImageHistoryFilter::destroying)
	{
		return;
	}

	//
	// Acquire a shared lock to iterate processes.
	//
	FltAcquirePushLockShared(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Iterate histories for a match.
	//
	if (ImageHistoryFilter::ProcessHistoryHead)
	{
		currentProcessHistory = ImageHistoryFilter::ProcessHistoryHead;
		do
		{
			if (currentProcessHistory->ProcessId == ProcessId)
			{
				break;
			}
			currentProcessHistory = RCAST<PPROCESS_HISTORY_ENTRY>(currentProcessHistory->ListEntry.Blink);
		} while (currentProcessHistory && currentProcessHistory != ImageHistoryFilter::ProcessHistoryHead);
	}

	//
	// This might happen if we load on a running machine that already has processes.
	//
	if (currentProcessHistory == NULL)
	{
		DBGPRINT("ImageHistoryFilter!LoadImageNotifyRoutine: Failed to find PID 0x%X in history.", ProcessId);
		goto Exit;
	}

	//
	// Allocate space for the new image history entry.
	//
	newImageLoadHistory = RCAST<PIMAGE_LOAD_HISTORY_ENTRY>(ExAllocatePoolWithTag(PagedPool, sizeof(IMAGE_LOAD_HISTORY_ENTRY), IMAGE_HISTORY_TAG));
	if (newImageLoadHistory == NULL)
	{
		DBGPRINT("ImageHistoryFilter!LoadImageNotifyRoutine: Failed to allocate space for the image history entry.");
		status = STATUS_NO_MEMORY;
		goto Exit;
	}

	//
	// Copy the image file name if it is provided.
	//
	if (FullImageName)
	{
		//
		// Allocate the copy buffer. FullImageName will not be valid forever.
		//
		newImageLoadHistory->ImageFileName.Buffer = RCAST<PWCH>(ExAllocatePoolWithTag(PagedPool, FullImageName->Length + 1, IMAGE_NAME_TAG));
		if (newImageLoadHistory->ImageFileName.Buffer == NULL)
		{
			DBGPRINT("ImageHistoryFilter!LoadImageNotifyRoutine: Failed to allocate space for the image file name.");
			status = STATUS_NO_MEMORY;
			goto Exit;
		}

		newImageLoadHistory->ImageFileName.Length = FullImageName->Length + 1;
		newImageLoadHistory->ImageFileName.MaximumLength = FullImageName->Length + 1;

		//
		// Copy the image name.
		//
		status = RtlStringCbCopyUnicodeString(newImageLoadHistory->ImageFileName.Buffer, FullImageName->Length + 1, FullImageName);
		if (NT_SUCCESS(status) == FALSE)
		{
			DBGPRINT("ImageHistoryFilter!LoadImageNotifyRoutine: Failed to copy the image file name with status 0x%X.", status);
			goto Exit;
		}
	}

	//
	// Grab the user-mode stack.
	//
	newImageLoadHistory->CallerStackHistorySize = walker.WalkAndResolveStack(tempStackReturns, MAX_STACK_RETURN_HISTORY);
	newImageLoadHistory->CallerStackHistory = RCAST<PSTACK_RETURN_INFO>(ExAllocatePoolWithTag(PagedPool, sizeof(STACK_RETURN_INFO) * newImageLoadHistory->CallerStackHistorySize, STACK_HISTORY_TAG));
	if (newImageLoadHistory->CallerStackHistory == NULL)
	{
		DBGPRINT("ImageHistoryFilter!LoadImageNotifyRoutine: Failed to allocate space for the stack history.");
		status = STATUS_NO_MEMORY;
		goto Exit;
	}
	memcpy(newImageLoadHistory->CallerStackHistory, tempStackReturns, sizeof(STACK_RETURN_INFO) * newImageLoadHistory->CallerStackHistorySize);

	FltAcquirePushLockExclusive(&currentProcessHistory->ImageLoadHistoryLock);

	//
	// Check if the list has been initialized.
	//
	if (currentProcessHistory->ImageLoadHistory == NULL)
	{
		currentProcessHistory->ImageLoadHistory = newImageLoadHistory;
		InitializeListHead(RCAST<PLIST_ENTRY>(currentProcessHistory->ImageLoadHistory));
	}
	//
	// Otherwise, just append the element to the end of the list.
	//
	else
	{
		InsertTailList(RCAST<PLIST_ENTRY>(currentProcessHistory->ImageLoadHistory), RCAST<PLIST_ENTRY>(newImageLoadHistory));
	}

	FltReleasePushLock(&currentProcessHistory->ImageLoadHistoryLock);
Exit:
	//
	// Release the lock.
	//
	FltReleasePushLock(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Clean up on failure.
	//
	if (newImageLoadHistory && NT_SUCCESS(status) == FALSE)
	{
		if (newImageLoadHistory->ImageFileName.Buffer)
		{
			ExFreePoolWithTag(newImageLoadHistory->ImageFileName.Buffer, IMAGE_NAME_TAG);
		}
		ExFreePoolWithTag(newImageLoadHistory, IMAGE_HISTORY_TAG);
	}
}

/**
	Get the summary for MaxProcessSummaries processes starting from the top of list + SkipCount.
	@param SkipCount - How many processes to skip in the list.
	@param ProcessSummaries - Caller-supplied array of process summaries that this function fills.
	@param MaxProcessSumaries - Maximum number of process summaries that the array allows for.
	@return The actual number of summaries returned.
*/
USHORT
ImageHistoryFilter::GetProcessHistorySummary (
	_In_ USHORT SkipCount,
	_Inout_ PROCESS_SUMMARY_ENTRY ProcessSummaries[],
	_In_ USHORT MaxProcessSummaries
	)
{
	PPROCESS_HISTORY_ENTRY currentProcessHistory;
	USHORT currentProcessIndex;
	USHORT actualFilledSummaries;
	NTSTATUS status;

	currentProcessIndex = 0;
	actualFilledSummaries = 0;

	if (ImageHistoryFilter::destroying)
	{
		return 0;
	}

	//
	// Acquire a shared lock to iterate processes.
	//
	FltAcquirePushLockShared(&ImageHistoryFilter::ProcessHistoryLock);

	//
	// Iterate histories for the MaxProcessSummaries processes after SkipCount processes.
	//
	if (ImageHistoryFilter::ProcessHistoryHead)
	{
		currentProcessHistory = ImageHistoryFilter::ProcessHistoryHead;
		do
		{
			if (currentProcessIndex >= SkipCount)
			{
				//
				// Fill out the summary.
				//
				ProcessSummaries[currentProcessIndex].EpochExecutionTime = currentProcessHistory->EpochExecutionTime;
				ProcessSummaries[currentProcessIndex].ProcessId = currentProcessHistory->ProcessId;
				ProcessSummaries[currentProcessIndex].ProcessTerminated = currentProcessHistory->ProcessTerminated;
				
				if (currentProcessHistory->ProcessImageFileName)
				{
					//
					// Copy the image name.
					//
					status = RtlStringCbCopyUnicodeString(RCAST<NTSTRSAFE_PWSTR>(&ProcessSummaries[currentProcessIndex].ImageFileName), MAX_PATH, currentProcessHistory->ProcessImageFileName);
					if (NT_SUCCESS(status) == FALSE)
					{
						DBGPRINT("ImageHistoryFilter!GetProcessHistorySummary: Failed to copy the image file name with status 0x%X.", status);
						break;
					}
				}
				actualFilledSummaries++;
			}
			currentProcessIndex++;
			currentProcessHistory = RCAST<PPROCESS_HISTORY_ENTRY>(currentProcessHistory->ListEntry.Blink);
		} while (currentProcessHistory && currentProcessHistory != ImageHistoryFilter::ProcessHistoryHead && MaxProcessSummaries > actualFilledSummaries);
	}

	//
	// Release the lock.
	//
	FltReleasePushLock(&ImageHistoryFilter::ProcessHistoryLock);

	return actualFilledSummaries;
}