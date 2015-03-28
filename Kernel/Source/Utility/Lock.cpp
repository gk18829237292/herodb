#include "Lock.h"

namespace vl
{
	namespace database
	{
		using namespace collections;

#define LOCK_TYPES ((vint)LockTargetAccess::NumbersOfLockTypes)

/***********************************************************************
LockManager (ObjectLock)
***********************************************************************/

		const bool lockCompatibility
			[LOCK_TYPES] // Request
			[LOCK_TYPES] // Existing
		= {
			{true,	true,	true,	true,	true,	false},
			{true,	true,	true,	false,	false,	false},
			{true,	true,	false,	false,	false,	false},
			{true,	false,	false,	true,	false,	false},
			{true,	false,	false,	false,	false,	false},
			{false,	false,	false,	false,	false,	false},
		};

		template<typename TInfo>
		bool LockManager::AcquireObjectLockUnsafe(
			Ptr<TInfo> lockInfo,
			Ptr<TransInfo> owner,
			const LockTarget& target
			)
		{
			vint access = (vint)target.access;
			for(vint i = 0; i < LOCK_TYPES; i++)
			{
				if (lockCompatibility[access][i] == false)
				{
					if (lockInfo->acquiredLocks[i] > 0)
					{
						return false;
					}
				}
			}

			INCRC(&lockInfo->acquiredLocks[access]);
			owner->acquiredLocks.Add(target);
			return true;
		}

		template<typename TInfo>
		bool LockManager::ReleaseObjectLockUnsafe(
			Ptr<TInfo> lockInfo,
			Ptr<TransInfo> owner,
			const LockTarget& target
			)
		{
			auto index = owner->acquiredLocks.IndexOf(target);
			if (index < 0)
			{
				return false;
			}

			auto result = DECRC(&lockInfo->acquiredLocks[(vint)target.access]);
			CHECK_ERROR(result >= 0, L"vl::database::LockManager::ReleaseObjectLock(Ptr<TInfo>, Ptr<TransInfo>, const LockTarget&)#Internal error: TInfo::acquiredLocks is corrupted.");
			return owner->acquiredLocks.RemoveAt(index);
		}
		
		Ptr<LockManager::TransInfo> LockManager::CheckInputUnsafe(BufferTransaction owner, const LockTarget& target)
		{
			if (!owner.IsValid()) return nullptr;
			if (!target.table.IsValid()) return nullptr;
			switch (target.type)
			{
			case LockTargetType::Page:
				if (!target.page.IsValid()) return nullptr;
				break;
			case LockTargetType::Row:
				if (!target.address.IsValid()) return nullptr;
				break;
			default:;
			}

			if (!tables.Keys().Contains(target.table))
			{
				return nullptr;
			}

			vint index = transactions.Keys().IndexOf(owner);
			if (index == -1)
			{
				return nullptr;
			}

			return transactions.Values()[index];;
		}


		bool LockManager::AddPendingLockUnsafe(Ptr<TransInfo> owner, const LockTarget& target)
		{
			if (owner->pendingLock.IsValid())
			{
				return false;
			}

			vint index = pendingTransactions.IndexOf(owner->trans);
			if (index != -1)
			{
				return false;
			}

			pendingTransactions.Add(owner->trans);
			owner->pendingLock = target;
			return true;
		}

		bool LockManager::RemovePendingLockUnsafe(Ptr<TransInfo> owner, const LockTarget& target)
		{
			if (!owner->pendingLock.IsValid() || owner->pendingLock != target)
			{
				return false;
			}

			vint index = pendingTransactions.IndexOf(owner->trans);
			if (index == -1)
			{
				return false;
			}

			pendingTransactions.RemoveAt(index);
			owner->pendingLock = LockTarget();
			return true;
		}

/***********************************************************************
LockManager (Template)
***********************************************************************/

		const LockTarget& GetLockTarget(const LockTarget& target)
		{
			return target;
		}

		template<typename... T>
		const LockTarget& GetLockTarget(Tuple<const LockTarget&, T...> arguments)
		{
			return arguments.f0;
		}

		template<typename TArgs>
		bool LockManager::OperateObjectLock(
			BufferTransaction owner,
			TArgs arguments,
			TableLockHandler<TArgs> tableLockHandler,
			PageLockHandler<TArgs> pageLockHandler,
			RowLockHandler<TArgs> rowLockHandler,
			bool createLockInfo,
			bool checkPendingLock
			)
		{
			const LockTarget& target = GetLockTarget(arguments);
			Ptr<TableLockInfo> tableLockInfo;
			Ptr<PageLockInfo> pageLockInfo;
			Ptr<RowLockInfo> rowLockInfo;
			BufferPage targetPage;
			vuint64_t targetOffset = ~(vuint64_t)0;
			vint index = -1;

			SPIN_LOCK(lock)
			{
				///////////////////////////////////////////////////////////
				// Check Input
				///////////////////////////////////////////////////////////

				auto transInfo = CheckInputUnsafe(owner, target);
				if (!transInfo) return false;
				if (checkPendingLock && transInfo->pendingLock.IsValid())
				{
					return false;
				}

				///////////////////////////////////////////////////////////
				// Find TableLock
				///////////////////////////////////////////////////////////

				if (tableLocks.Count() <= target.table.index)
				{
					if (!createLockInfo)
					{
						return false;
					}
					tableLocks.Resize(target.table.index + 1);
				}

				tableLockInfo = tableLocks[target.table.index];
				if (!tableLockInfo)
				{
					if (!createLockInfo)
					{
						return false;
					}
					tableLockInfo = new TableLockInfo(target.table);
					tableLocks[target.table.index] = tableLockInfo;
				}

				///////////////////////////////////////////////////////////
				// Process TableLock
				///////////////////////////////////////////////////////////

				switch (target.type)
				{
				case LockTargetType::Table:
					return (this->*tableLockHandler)(transInfo, arguments, tableLockInfo);
				case LockTargetType::Page:
					targetPage = target.page;
					break;
				case LockTargetType::Row:
					CHECK_ERROR(bm->DecodePointer(target.address, targetPage, targetOffset), L"vl::database::LockManager::AcquireLock(BufferTransaction, const LockTarget&, LockResult&)#Internal error: Unable to decode row pointer.");
					break;
				}

				///////////////////////////////////////////////////////////
				// Find PageLock
				///////////////////////////////////////////////////////////

				index = tableLockInfo->pageLocks.Keys().IndexOf(targetPage);
				if (index == -1)
				{
					if (!createLockInfo)
					{
						return false;
					}
					pageLockInfo = new PageLockInfo(target.page);
					tableLockInfo->pageLocks.Add(targetPage, pageLockInfo);
				}
				else
				{
					pageLockInfo = tableLockInfo->pageLocks.Values()[index];
				}

				///////////////////////////////////////////////////////////
				// Process PageLock
				///////////////////////////////////////////////////////////

				if (target.type == LockTargetType::Page)
				{
					return (this->*pageLockHandler)(transInfo, arguments, tableLockInfo, pageLockInfo);
				}

				///////////////////////////////////////////////////////////
				// Find RowLock
				///////////////////////////////////////////////////////////

				index = pageLockInfo->rowLocks.Keys().IndexOf(targetOffset);
				if (index == -1)
				{
					if (!createLockInfo)
					{
						return false;
					}
					rowLockInfo = new RowLockInfo(targetOffset);
					pageLockInfo->rowLocks.Add(targetOffset, rowLockInfo);
				}
				else
				{
					rowLockInfo = pageLockInfo->rowLocks.Values()[index];
				}

				///////////////////////////////////////////////////////////
				// Process RowLock
				///////////////////////////////////////////////////////////

				if (target.type == LockTargetType::Row)
				{
					return (this->*rowLockHandler)(transInfo, arguments, tableLockInfo, pageLockInfo, rowLockInfo);
				}
			}

			return false;
		}

/***********************************************************************
LockManager (Acquire)
***********************************************************************/
		template<typename TLockInfo>
		bool LockManager::AcquireGeneralLock(Ptr<TransInfo> owner, AcquireLockArgs arguments, Ptr<TLockInfo> lockInfo)
		{
			const LockTarget& target = arguments.f0;
			LockResult& result = arguments.f1;

			if (AcquireObjectLockUnsafe(lockInfo, owner, target))
			{
				result.blocked = false;
			}
			else
			{
				result.blocked = true;
				SPIN_LOCK(lock)
				{
					return AddPendingLockUnsafe(owner, target);
				}
			}
			return true;
		}

		bool LockManager::AcquireTableLock(Ptr<TransInfo> owner, AcquireLockArgs arguments, Ptr<TableLockInfo> tableLockInfo)
		{
			return AcquireGeneralLock(owner, arguments, tableLockInfo);
		}

		bool LockManager::AcquirePageLock(Ptr<TransInfo> owner, AcquireLockArgs arguments, Ptr<TableLockInfo> tableLockInfo, Ptr<PageLockInfo> pageLockInfo)
		{
			return AcquireGeneralLock(owner, arguments, pageLockInfo);
		}

		bool LockManager::AcquireRowLock(Ptr<TransInfo> owner, AcquireLockArgs arguments, Ptr<TableLockInfo> tableLockInfo, Ptr<PageLockInfo> pageLockInfo, Ptr<RowLockInfo> rowLockInfo)
		{
			return AcquireGeneralLock(owner, arguments, rowLockInfo);
		}

/***********************************************************************
LockManager (Release)
***********************************************************************/

		bool LockManager::ReleaseTableLock(Ptr<TransInfo> owner, ReleaseLockArgs arguments, Ptr<TableLockInfo> tableLockInfo)
		{
			const LockTarget& target = arguments;

			if (!ReleaseObjectLockUnsafe(tableLockInfo, owner, target))
			{
				SPIN_LOCK(lock)
				{
					return RemovePendingLockUnsafe(owner, target);
				}
			}
			return true;
		}

		bool LockManager::ReleasePageLock(Ptr<TransInfo> owner, ReleaseLockArgs arguments, Ptr<TableLockInfo> tableLockInfo, Ptr<PageLockInfo> pageLockInfo)
		{
			const LockTarget& target = arguments;

			bool success = true;
			if (!ReleaseObjectLockUnsafe(pageLockInfo, owner, target))
			{
				success = RemovePendingLockUnsafe(owner, target);
			}

			if (pageLockInfo->IsEmpty())
			{
				tableLockInfo->pageLocks.Remove(pageLockInfo->object);
			}
			return success;
		}

		bool LockManager::ReleaseRowLock(Ptr<TransInfo> owner, ReleaseLockArgs arguments, Ptr<TableLockInfo> tableLockInfo, Ptr<PageLockInfo> pageLockInfo, Ptr<RowLockInfo> rowLockInfo)
		{
			const LockTarget& target = arguments;

			bool success = true;
			if (!ReleaseObjectLockUnsafe(rowLockInfo, owner, target))
			{
				success = RemovePendingLockUnsafe(owner, target);
			}

			if (rowLockInfo->IsEmpty())
			{
				pageLockInfo->rowLocks.Remove(rowLockInfo->object);
				if (pageLockInfo->IsEmpty())
				{
					tableLockInfo->pageLocks.Remove(pageLockInfo->object);
				}
			}
			return success;
		}

/***********************************************************************
LockManager (Upgrade)
***********************************************************************/

		template<typename TLockInfo>
		bool LockManager::UpgradeGeneralLock(Ptr<TransInfo> owner, UpgradeLockArgs arguments, Ptr<TLockInfo> lockInfo)
		{
			const LockTarget& oldTarget = arguments.f0;
			LockTargetAccess newAccess = arguments.f1;
			LockResult& result = arguments.f2;

			if (!ReleaseObjectLockUnsafe(lockInfo, owner, oldTarget))
			{
				return false;
			}
			else
			{
				LockTarget newTarget = oldTarget;
				newTarget.access = newAccess;
				AcquireLockArgs newArguments(newTarget, result);
				return AcquireGeneralLock(owner, newArguments, lockInfo);
			}
		}

		bool LockManager::UpgradeTableLock(Ptr<TransInfo> owner, UpgradeLockArgs arguments, Ptr<TableLockInfo> tableLockInfo)
		{
			return UpgradeGeneralLock(owner, arguments, tableLockInfo);
		}

		bool LockManager::UpgradePageLock(Ptr<TransInfo> owner, UpgradeLockArgs arguments, Ptr<TableLockInfo> tableLockInfo, Ptr<PageLockInfo> pageLockInfo)
		{
			return UpgradeGeneralLock(owner, arguments, pageLockInfo);
		}

		bool LockManager::UpgradeRowLock(Ptr<TransInfo> owner, UpgradeLockArgs arguments, Ptr<TableLockInfo> tableLockInfo, Ptr<PageLockInfo> pageLockInfo, Ptr<RowLockInfo> rowLockInfo)
		{
			return UpgradeGeneralLock(owner, arguments, rowLockInfo);
		}

/***********************************************************************
LockManager
***********************************************************************/

		LockManager::LockManager(BufferManager* _bm)
			:bm(_bm)
		{
		}

		LockManager::~LockManager()
		{
		}

		bool LockManager::RegisterTable(BufferTable table, BufferSource source)
		{
			SPIN_LOCK(lock)
			{
				if (tables.Keys().Contains(table))
				{
					return false;
				}

				if (!bm->GetIndexPage(source).IsValid())
				{
					return false;
				}

				auto info = MakePtr<TableInfo>();
				info->table = table;
				info->source = source;
				tables.Add(table, info);
			}
			return true;
		}

		bool LockManager::UnregisterTable(BufferTable table)
		{
			SPIN_LOCK(lock)
			{
				if (!tables.Keys().Contains(table))
				{
					return false;
				}

				tables.Remove(table);
			}
			return true;
		}

		bool LockManager::RegisterTransaction(BufferTransaction trans, vuint64_t importance)
		{
			SPIN_LOCK(lock)
			{
				if (transactions.Keys().Contains(trans))
				{
					return false;
				}

				auto info = MakePtr<TransInfo>();
				info->trans = trans;
				info->importance = importance;
				transactions.Add(trans, info);
			}
			return true;
		}

		bool LockManager::UnregisterTransaction(BufferTransaction trans)
		{
			SPIN_LOCK(lock)
			{
				if (!transactions.Keys().Contains(trans))
				{
					return false;
				}

				transactions.Remove(trans);
			}
			return true;
		}

		bool LockManager::AcquireLock(BufferTransaction owner, const LockTarget& target, LockResult& result)
		{
			AcquireLockArgs arguments(target, result);
			return OperateObjectLock<AcquireLockArgs>(
				owner,
				arguments,
				&LockManager::AcquireTableLock,
				&LockManager::AcquirePageLock,
				&LockManager::AcquireRowLock,
				true,
				true
				);
		}

		bool LockManager::ReleaseLock(BufferTransaction owner, const LockTarget& target)
		{
			ReleaseLockArgs arguments = target;
			LockResult result;
			return OperateObjectLock<ReleaseLockArgs>(
				owner,
				arguments,
				&LockManager::ReleaseTableLock,
				&LockManager::ReleasePageLock,
				&LockManager::ReleaseRowLock,
				false,
				false
				);
		}

		bool LockManager::UpgradeLock(BufferTransaction owner, const LockTarget& oldTarget, LockTargetAccess newAccess, LockResult& result)
		{
			UpgradeLockArgs arguments(oldTarget, newAccess, result);
			return OperateObjectLock<UpgradeLockArgs>(
				owner,
				arguments,
				&LockManager::UpgradeTableLock,
				&LockManager::UpgradePageLock,
				&LockManager::UpgradeRowLock,
				false,
				true
				);
		}

		bool LockManager::TableHasLocks(BufferTable table)
		{
			if (!table.IsValid()) return false;

			Ptr<TableLockInfo> tableLockInfo;
			SPIN_LOCK(lock)
			{
				if (tableLocks.Count() <= table.index)
				{
					return false;
				}
				tableLockInfo = tableLocks[table.index];
				return !tableLockInfo->IsEmpty();
			}
			return false;
		}

		BufferTransaction LockManager::PickTransaction(LockResult& result)
		{
			return BufferTransaction::Invalid();
		}

		void LockManager::DetectDeadlock(DeadlockInfo::List& infos)
		{
		}

		bool LockManager::Rollback(BufferTransaction trans)
		{
			return false;
		}
		
#undef LOCK_TYPES
	}
}
